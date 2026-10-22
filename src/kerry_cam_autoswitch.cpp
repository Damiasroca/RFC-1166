#include "kerry_cam_autoswitch.h"

#include "kerry_cam_exposure.h"
#include "kerry_cam_exposure_store.h"
#include "kerry_cam_luma.h"
#include "kerry_log.h"
#include "kerry_shared.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "autosw";

typedef struct {
  uint8_t version;
  uint8_t enabled;
  uint8_t highLuma;
  uint8_t lowLuma;
  uint8_t intervalMin;
  uint8_t samples;
} kerry_cam_autoswitch_blob_t;

static kerry_cam_autoswitch_cfg_t g_cfg = {false, 150, 40, 2, 3};
static kerry_cam_mode_t g_mode = KERRY_CAM_MODE_UNKNOWN;
static uint8_t g_lowCount = 0;
static uint8_t g_highCount = 0;
static kerry_luma_t g_lastLuma = {};
static bool g_haveLast = false;
static uint32_t g_lastSampleMs = 0;
static uint32_t g_nextSampleMs = 0;
static const char *g_lastSwitchReason = "";
static bool g_applying = false;
static SemaphoreHandle_t g_mu = nullptr;

static kerry_cam_autoswitch_cfg_t defaultCfg() {
  kerry_cam_autoswitch_cfg_t cfg = {};
  cfg.enabled = false;
  cfg.highLuma = 150;
  cfg.lowLuma = 40;
  cfg.intervalMin = 2;
  cfg.samples = 3;
  return cfg;
}

static const char *modeName(kerry_cam_mode_t mode) {
  if (mode == KERRY_CAM_MODE_DAY) return "day";
  if (mode == KERRY_CAM_MODE_NIGHT) return "night";
  return "unknown";
}

static const char *validateCfg(const kerry_cam_autoswitch_cfg_t *cfg) {
  if (!cfg) return "Missing settings";
  if ((unsigned)cfg->lowLuma + 10u > cfg->highLuma || cfg->highLuma > 250) return "Luma thresholds out of range";
  if (cfg->intervalMin < 1 || cfg->intervalMin > 60) return "intervalMin out of range";
  if (cfg->samples < 1 || cfg->samples > 10) return "samples out of range";
  return nullptr;
}

static void ensureMu() {
  if (!g_mu) g_mu = xSemaphoreCreateMutex();
}

static void saveCfgNvs(const kerry_cam_autoswitch_cfg_t *cfg) {
  kerry_cam_autoswitch_blob_t blob = {};
  blob.version = 1;
  blob.enabled = cfg->enabled ? 1 : 0;
  blob.highLuma = cfg->highLuma;
  blob.lowLuma = cfg->lowLuma;
  blob.intervalMin = cfg->intervalMin;
  blob.samples = cfg->samples;
  Preferences prefs;
  prefs.begin("kautosw", false);
  prefs.putBytes("cfg", &blob, sizeof(blob));
  prefs.end();
}

static void saveModeNvs(kerry_cam_mode_t mode) {
  Preferences prefs;
  prefs.begin("kautosw", false);
  prefs.putUChar("mode", (uint8_t)mode);
  prefs.end();
}

static void resetCounts() {
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_lowCount = 0;
  g_highCount = 0;
  xSemaphoreGive(g_mu);
}

static bool beginApply() {
  xSemaphoreTake(g_mu, portMAX_DELAY);
  if (g_applying) {
    xSemaphoreGive(g_mu);
    return false;
  }
  g_applying = true;
  xSemaphoreGive(g_mu);
  return true;
}

static void endApply() {
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_applying = false;
  xSemaphoreGive(g_mu);
}

static void fillExp(JsonObject obj, const kerry_cam_exp_cfg_t *cfg) {
  obj["aecOn"] = cfg->aecOn;
  obj["aecValue"] = cfg->aecValue;
  obj["aec2On"] = cfg->aec2On;
  obj["aeLevel"] = cfg->aeLevel;
  obj["agcOn"] = cfg->agcOn;
  obj["agcGain"] = cfg->agcGain;
  obj["gainCeiling"] = cfg->gainCeiling;
  obj["clkrc"] = cfg->clkrc;
  obj["dummyLines"] = cfg->dummyLines;
}

void kerryCamAutoSwitchLoad() {
  ensureMu();
  kerry_cam_autoswitch_cfg_t cfg = defaultCfg();
  kerry_cam_mode_t mode = KERRY_CAM_MODE_UNKNOWN;
  bool bad = false;
  Preferences prefs;
  prefs.begin("kautosw", true);
  size_t n = prefs.getBytesLength("cfg");
  if (n == 0) {
    bad = false;
  } else if (n != sizeof(kerry_cam_autoswitch_blob_t)) {
    bad = true;
  } else {
    kerry_cam_autoswitch_blob_t blob = {};
    if (prefs.getBytes("cfg", &blob, sizeof(blob)) != sizeof(blob) || blob.version != 1) {
      bad = true;
    } else {
      cfg.enabled = blob.enabled != 0;
      cfg.highLuma = blob.highLuma;
      cfg.lowLuma = blob.lowLuma;
      cfg.intervalMin = blob.intervalMin;
      cfg.samples = blob.samples;
      if (validateCfg(&cfg)) {
        cfg = defaultCfg();
        bad = true;
      }
    }
  }
  uint8_t storedMode = prefs.getUChar("mode", KERRY_CAM_MODE_UNKNOWN);
  if (storedMode == KERRY_CAM_MODE_DAY || storedMode == KERRY_CAM_MODE_NIGHT) mode = (kerry_cam_mode_t)storedMode;
  prefs.end();
  if (bad) KERRY_LOGW(TAG, "stored switching settings are invalid, using defaults");

  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_cfg = cfg;
  g_mode = mode;
  g_lowCount = 0;
  g_highCount = 0;
  g_haveLast = false;
  g_nextSampleMs = 0;
  g_lastSwitchReason = "";
  xSemaphoreGive(g_mu);
}

const char *kerryCamAutoSwitchSaveNight() {
  kerry_cam_exp_cfg_t cfg;
  kerryCamExpStoreGet(&cfg);
  kerryCamExpStoreSetNight(&cfg);
  return nullptr;
}

const char *kerryCamAutoSwitchApply(kerry_cam_mode_t mode) {
  if (mode != KERRY_CAM_MODE_DAY && mode != KERRY_CAM_MODE_NIGHT) return "Unknown mode";
  if (!beginApply()) return "Switch already in progress";
  kerry_cam_exp_cfg_t night;
  if (!kerryCamExpStoreGetNight(&night)) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_lastSwitchReason = "No night preset";
    g_lowCount = 0;
    g_highCount = 0;
    xSemaphoreGive(g_mu);
    endApply();
    return "No night preset";
  }
  kerry_cam_exp_cfg_t cfg;
  if (mode == KERRY_CAM_MODE_NIGHT) cfg = night;
  else kerryCamExposureDayCfg(&night, &cfg);
  const char *err = kerryCamApplyExposureCfg(&cfg);
  if (err) {
    KERRY_LOGW(TAG, "switch to %s failed: %s", modeName(mode), err);
    resetCounts();
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_lastSwitchReason = err;
    xSemaphoreGive(g_mu);
    endApply();
    return err;
  }
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_mode = mode;
  g_lowCount = 0;
  g_highCount = 0;
  g_lastSwitchReason = mode == KERRY_CAM_MODE_NIGHT ? "night" : "day";
  xSemaphoreGive(g_mu);
  saveModeNvs(mode);
  KERRY_LOGI(TAG, "switched to %s", modeName(mode));
  endApply();
  return nullptr;
}

const char *kerryCamAutoSwitchSetCfg(const kerry_cam_autoswitch_cfg_t *cfg) {
  const char *err = validateCfg(cfg);
  if (err) return err;
  ensureMu();
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_cfg = *cfg;
  g_lowCount = 0;
  g_highCount = 0;
  g_nextSampleMs = millis();
  kerry_cam_autoswitch_cfg_t copy = g_cfg;
  xSemaphoreGive(g_mu);
  saveCfgNvs(&copy);
  return nullptr;
}

void kerryCamAutoSwitchFillJson(JsonDocument &doc) {
  ensureMu();
  kerry_cam_autoswitch_cfg_t cfg;
  kerry_cam_mode_t mode;
  uint8_t lowCount;
  uint8_t highCount;
  kerry_luma_t last;
  bool haveLast;
  uint32_t lastSampleMs;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  cfg = g_cfg;
  mode = g_mode;
  lowCount = g_lowCount;
  highCount = g_highCount;
  last = g_lastLuma;
  haveLast = g_haveLast;
  lastSampleMs = g_lastSampleMs;
  xSemaphoreGive(g_mu);

  doc["enabled"] = cfg.enabled;
  doc["highLuma"] = cfg.highLuma;
  doc["lowLuma"] = cfg.lowLuma;
  doc["intervalMin"] = cfg.intervalMin;
  doc["samples"] = cfg.samples;
  doc["mode"] = modeName(mode);
  doc["lowCount"] = lowCount;
  doc["highCount"] = highCount;
  if (haveLast) {
    JsonObject luma = doc["lastLuma"].to<JsonObject>();
    luma["avg"] = last.avg;
    luma["min"] = last.min;
    luma["max"] = last.max;
    doc["lastSampleAgeS"] = (uint32_t)((millis() - lastSampleMs) / 1000UL);
  } else {
    doc["lastLuma"] = nullptr;
    doc["lastSampleAgeS"] = nullptr;
  }

  kerry_cam_exp_cfg_t night;
  bool hasNight = kerryCamExpStoreGetNight(&night);
  doc["hasNight"] = hasNight;
  if (hasNight) fillExp(doc["night"].to<JsonObject>(), &night);
  else doc["night"] = nullptr;
}

static const char *switchForLuma(kerry_cam_mode_t mode, const kerry_luma_t *luma) {
  const char *err = kerryCamAutoSwitchApply(mode);
  if (!err && luma) {
    KERRY_LOGI(TAG, "switch trigger luma avg=%u min=%u max=%u", (unsigned)luma->avg, (unsigned)luma->min,
               (unsigned)luma->max);
  }
  return err;
}

void kerryCamAutoSwitchService(uint32_t nowMs) {
  ensureMu();
  kerry_cam_autoswitch_cfg_t cfg;
  uint32_t nextMs;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  cfg = g_cfg;
  nextMs = g_nextSampleMs;
  xSemaphoreGive(g_mu);
  if (!cfg.enabled) return;
  kerry_cam_exp_cfg_t night;
  if (!kerryCamExpStoreGetNight(&night)) return;
  if ((int32_t)(nowMs - nextMs) < 0) return;
  if (!kerryCamSamplingAllowed()) return;

  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_nextSampleMs = nowMs + (uint32_t)cfg.intervalMin * 60000UL;
  xSemaphoreGive(g_mu);

  kerry_luma_t luma;
  if (!kerryCamSampleLuma(&luma)) {
    if (kerry_log_every_ms("autosw_sample", 60000)) KERRY_LOGW(TAG, "luma sample failed");
    return;
  }

  uint8_t lowCount = 0;
  uint8_t highCount = 0;
  bool toNight = false;
  bool toDay = false;
  kerry_cam_mode_t mode;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_lastLuma = luma;
  g_haveLast = true;
  g_lastSampleMs = nowMs;
  if (g_mode != KERRY_CAM_MODE_NIGHT && luma.avg <= cfg.lowLuma) {
    if (g_lowCount < 255) g_lowCount++;
  } else {
    g_lowCount = 0;
  }
  if (g_mode != KERRY_CAM_MODE_DAY && luma.avg >= cfg.highLuma) {
    if (g_highCount < 255) g_highCount++;
  } else {
    g_highCount = 0;
  }
  lowCount = g_lowCount;
  highCount = g_highCount;
  mode = g_mode;
  toNight = g_lowCount >= cfg.samples;
  toDay = g_highCount >= cfg.samples;
  xSemaphoreGive(g_mu);

  KERRY_LOGI(TAG, "luma avg=%u min=%u max=%u mode=%s low=%u high=%u", (unsigned)luma.avg, (unsigned)luma.min,
             (unsigned)luma.max, modeName(mode), (unsigned)lowCount, (unsigned)highCount);
  if (toNight) switchForLuma(KERRY_CAM_MODE_NIGHT, &luma);
  else if (toDay) switchForLuma(KERRY_CAM_MODE_DAY, &luma);
}
