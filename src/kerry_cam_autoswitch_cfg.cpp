#include "kerry_cam_autoswitch_int.h"

#include "kerry_cam_exposure_store.h"
#include "kerry_log.h"

#include <Arduino.h>
#include <Preferences.h>

static const char *TAG = "autosw";

typedef struct {
  uint8_t version;
  uint8_t enabled;
  int16_t lowEvTenths;
  int16_t highEvTenths;
  uint8_t intervalMin;
  uint8_t samples;
  uint8_t minDwellMin;
} kerry_cam_autoswitch_blob_t;

kerry_cam_autoswitch_cfg_t kerryAutoDefaultCfg() {
  kerry_cam_autoswitch_cfg_t cfg = {};
  cfg.enabled = false;
  cfg.lowEvTenths = -60;
  cfg.highEvTenths = -40;
  cfg.intervalMin = 2;
  cfg.samples = 3;
  cfg.minDwellMin = 15;
  return cfg;
}

const char *kerryAutoValidateCfg(const kerry_cam_autoswitch_cfg_t *cfg) {
  if (!cfg) return "Missing settings";
  if (cfg->lowEvTenths < -200 || cfg->lowEvTenths > 200 || cfg->highEvTenths < -200 || cfg->highEvTenths > 200 ||
      (int)cfg->lowEvTenths + 10 > (int)cfg->highEvTenths)
    return "EV thresholds out of range";
  if (cfg->intervalMin < 1 || cfg->intervalMin > 60) return "intervalMin out of range";
  if (cfg->samples < 1 || cfg->samples > 10) return "samples out of range";
  if (cfg->minDwellMin > 240) return "minDwellMin out of range";
  return nullptr;
}

void kerryAutoSaveCfg(const kerry_cam_autoswitch_cfg_t *cfg) {
  kerry_cam_autoswitch_blob_t blob = {};
  blob.version = 2;
  blob.enabled = cfg->enabled ? 1 : 0;
  blob.lowEvTenths = cfg->lowEvTenths;
  blob.highEvTenths = cfg->highEvTenths;
  blob.intervalMin = cfg->intervalMin;
  blob.samples = cfg->samples;
  blob.minDwellMin = cfg->minDwellMin;
  Preferences prefs;
  prefs.begin("kautosw", false);
  prefs.putBytes("cfg", &blob, sizeof(blob));
  prefs.end();
}

void kerryAutoSaveMode(kerry_cam_mode_t mode) {
  Preferences prefs;
  prefs.begin("kautosw", false);
  prefs.putUChar("mode", (uint8_t)mode);
  prefs.end();
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
  kerryAutoEnsureMu();
  kerry_cam_autoswitch_cfg_t cfg = kerryAutoDefaultCfg();
  kerry_cam_mode_t mode = KERRY_CAM_MODE_UNKNOWN;
  bool reset = false;
  Preferences prefs;
  prefs.begin("kautosw", true);
  size_t n = prefs.getBytesLength("cfg");
  if (n == sizeof(kerry_cam_autoswitch_blob_t)) {
    kerry_cam_autoswitch_blob_t blob = {};
    if (prefs.getBytes("cfg", &blob, sizeof(blob)) == sizeof(blob) && blob.version == 2) {
      cfg.enabled = blob.enabled != 0;
      cfg.lowEvTenths = blob.lowEvTenths;
      cfg.highEvTenths = blob.highEvTenths;
      cfg.intervalMin = blob.intervalMin;
      cfg.samples = blob.samples;
      cfg.minDwellMin = blob.minDwellMin;
      if (kerryAutoValidateCfg(&cfg)) reset = true;
    } else {
      reset = true;
    }
  } else if (n != 0) {
    reset = true;
  }
  uint8_t storedMode = prefs.getUChar("mode", KERRY_CAM_MODE_UNKNOWN);
  if (storedMode == KERRY_CAM_MODE_DAY || storedMode == KERRY_CAM_MODE_NIGHT) mode = (kerry_cam_mode_t)storedMode;
  prefs.end();
  if (reset) {
    cfg = kerryAutoDefaultCfg();
    cfg.enabled = false;
    kerryAutoSaveCfg(&cfg);
    KERRY_LOGW(TAG, "switching settings were reset because the format changed");
  }

  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  g_autoCfg = cfg;
  g_autoMode = mode;
  g_autoLowCount = 0;
  g_autoHighCount = 0;
  g_autoHaveLast = false;
  g_autoHaveScene = false;
  g_autoHaveLastSwitch = false;
  g_autoNextSampleMs = 0;
  g_autoLastSwitchReason = "";
  xSemaphoreGive(g_autoMu);
}

void kerryCamAutoSwitchFillJson(JsonDocument &doc) {
  kerryAutoEnsureMu();
  kerry_cam_autoswitch_cfg_t cfg;
  kerry_cam_mode_t mode;
  uint8_t lowCount;
  uint8_t highCount;
  kerry_luma_t last;
  bool haveLast;
  uint32_t lastSampleMs;
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  cfg = g_autoCfg;
  mode = g_autoMode;
  lowCount = g_autoLowCount;
  highCount = g_autoHighCount;
  last = g_autoLastLuma;
  haveLast = g_autoHaveLast;
  lastSampleMs = g_autoLastSampleMs;
  xSemaphoreGive(g_autoMu);

  doc["enabled"] = cfg.enabled;
  doc["lowEv"] = (float)cfg.lowEvTenths / 10.0f;
  doc["highEv"] = (float)cfg.highEvTenths / 10.0f;
  doc["intervalMin"] = cfg.intervalMin;
  doc["samples"] = cfg.samples;
  doc["minDwellMin"] = cfg.minDwellMin;
  doc["mode"] = kerryAutoModeName(mode);
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
