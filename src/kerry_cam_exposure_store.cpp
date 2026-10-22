#include "kerry_cam_exposure_store.h"

#include "kerry_cam_exposure_cfg.h"
#include "kerry_log.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static kerry_cam_exp_cfg_t g_cfg = kerryCamExposureDefaultCfg();
static kerry_cam_exp_cfg_t g_staged = kerryCamExposureDefaultCfg();
static kerry_cam_exp_cfg_t g_prevCfg = kerryCamExposureDefaultCfg();
static bool g_hasStaged = false;
static kerry_cam_exp_cfg_t g_lastGood = kerryCamExposureDefaultCfg();
static bool g_hasLastGood = false;
static bool g_unverified = false;
static uint8_t g_captureFailStreak = 0;
static volatile bool g_revertPending = false;
static bool g_appliedSinceActivate = false;
static kerry_cam_exp_cfg_t g_night = kerryCamExposureDefaultCfg();
static bool g_hasNight = false;
static SemaphoreHandle_t g_storeMu = nullptr;

static bool clkrcNeedsChange(uint8_t stored, uint8_t *mapped, bool *legacy) {
  *legacy = true;
  if (stored == 0x81) *mapped = KERRY_EXP_CLKRC_SENTINEL;
  else if (stored == 0x83) *mapped = 0x01;
  else if (stored == 0x87) *mapped = 0x03;
  else if (stored == 0x8F) *mapped = 0x07;
  else if (!kerryCamExposureClkrcAllowed(stored)) {
    *legacy = false;
    *mapped = KERRY_EXP_CLKRC_SENTINEL;
  } else {
    return false;
  }
  return true;
}

static void writeNvs(const kerry_cam_exp_cfg_t *cfg) {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putBool("exp_aec", cfg->aecOn);
  prefs.putUShort("exp_aecv", cfg->aecValue);
  prefs.putBool("exp_aec2", cfg->aec2On);
  prefs.putChar("exp_ael", cfg->aeLevel);
  prefs.putBool("exp_agc", cfg->agcOn);
  prefs.putUChar("exp_agcg", cfg->agcGain);
  prefs.putUChar("exp_gceil", cfg->gainCeiling);
  prefs.putUChar("exp_clkrc", cfg->clkrc);
  prefs.putUShort("exp_dummy", cfg->dummyLines);
  prefs.end();
}

static void writeNightNvs(const kerry_cam_exp_cfg_t *cfg) {
  kerry_cam_exp_preset_blob_t blob = {};
  blob.version = 1;
  blob.cfg = *cfg;
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putBytes("exp_night", &blob, sizeof(blob));
  prefs.end();
}

void kerryCamExpStoreGet(kerry_cam_exp_cfg_t *out) {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  *out = g_cfg;
  xSemaphoreGive(g_storeMu);
}

void kerryCamExpStoreSetStaged(const kerry_cam_exp_cfg_t *cfg) {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  g_staged = *cfg;
  g_hasStaged = true;
  xSemaphoreGive(g_storeMu);
}

bool kerryCamExpStoreGetNight(kerry_cam_exp_cfg_t *out) {
  if (!out) return false;
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  bool has = g_hasNight;
  if (has) *out = g_night;
  xSemaphoreGive(g_storeMu);
  return has;
}

void kerryCamExpStoreSetNight(const kerry_cam_exp_cfg_t *cfg) {
  if (!cfg) return;
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  g_night = *cfg;
  g_hasNight = true;
  kerry_cam_exp_cfg_t copy = g_night;
  xSemaphoreGive(g_storeMu);
  writeNightNvs(&copy);
}

bool kerryCamExpStoreReplace(const kerry_cam_exp_cfg_t *cfg, kerry_cam_exp_cfg_t *prev) {
  if (!cfg || !prev) return false;
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (g_hasStaged) {
    xSemaphoreGive(g_storeMu);
    return false;
  }
  *prev = g_cfg;
  g_cfg = *cfg;
  xSemaphoreGive(g_storeMu);
  return true;
}

void kerryCamExposureLoad() {
  if (!g_storeMu) g_storeMu = xSemaphoreCreateMutex();

  kerry_cam_exp_cfg_t loaded = kerryCamExposureDefaultCfg();
  Preferences prefs;
  prefs.begin("kerry", true);
  loaded.aecOn = prefs.getBool("exp_aec", true);
  loaded.aecValue = prefs.getUShort("exp_aecv", 600);
  loaded.aec2On = prefs.getBool("exp_aec2", false);
  loaded.aeLevel = prefs.getChar("exp_ael", 0);
  loaded.agcOn = prefs.getBool("exp_agc", true);
  loaded.agcGain = prefs.getUChar("exp_agcg", 15);
  loaded.gainCeiling = prefs.getUChar("exp_gceil", KERRY_EXP_GAIN_CEIL_SENTINEL);
  loaded.clkrc = prefs.getUChar("exp_clkrc", KERRY_EXP_CLKRC_SENTINEL);
  loaded.dummyLines = prefs.getUShort("exp_dummy", 0);

  kerry_cam_exp_cfg_t night = kerryCamExposureDefaultCfg();
  bool hasNight = false;
  bool nightBad = false;
  size_t nightLen = prefs.getBytesLength("exp_night");
  if (nightLen == 0) {
    hasNight = false;
  } else if (nightLen != sizeof(kerry_cam_exp_preset_blob_t)) {
    nightBad = true;
  } else {
    kerry_cam_exp_preset_blob_t blob = {};
    if (prefs.getBytes("exp_night", &blob, sizeof(blob)) != sizeof(blob) || blob.version != 1) nightBad = true;
    else {
      night = blob.cfg;
      hasNight = true;
    }
  }
  prefs.end();

  uint8_t mappedClk = loaded.clkrc;
  bool legacyClk = false;
  kerry_cam_exp_cfg_t toSave = loaded;
  bool needSave = clkrcNeedsChange(loaded.clkrc, &mappedClk, &legacyClk);
  if (needSave) {
    if (legacyClk) {
      KERRY_LOGW("exposure", "stored clkrc 0x%02X migrated to 0x%02X", (unsigned)loaded.clkrc, (unsigned)mappedClk);
    } else {
      KERRY_LOGW("exposure", "stored clkrc 0x%02X is not valid, using driver default", (unsigned)loaded.clkrc);
    }
    toSave.clkrc = mappedClk;
  }

  bool nightSave = false;
  if (hasNight) {
    uint8_t nightClk = night.clkrc;
    bool nightLegacy = false;
    if (clkrcNeedsChange(night.clkrc, &nightClk, &nightLegacy)) {
      if (nightLegacy) {
        KERRY_LOGW("exposure", "night preset clkrc 0x%02X migrated to 0x%02X", (unsigned)night.clkrc,
                   (unsigned)nightClk);
      } else {
        KERRY_LOGW("exposure", "night preset clkrc 0x%02X is not valid, using driver default", (unsigned)night.clkrc);
      }
      night.clkrc = nightClk;
      nightSave = true;
    }
  }

  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  g_cfg = needSave ? toSave : loaded;
  g_night = night;
  g_hasNight = hasNight;
  g_lastGood = kerryCamExposureDefaultCfg();
  g_hasLastGood = true;
  g_unverified = false;
  xSemaphoreGive(g_storeMu);

  if (needSave) writeNvs(&toSave);
  if (nightSave) writeNightNvs(&night);
  if (!hasNight) {
    KERRY_LOGI("exposure", "no night preset (%s)", nightBad ? "invalid stored preset" : "not stored");
  }

  KERRY_LOGI("exposure", "loaded aec=%d agc=%d clkrc=0x%02X dummy=%u", toSave.aecOn ? 1 : 0, toSave.agcOn ? 1 : 0,
             (unsigned)toSave.clkrc, (unsigned)toSave.dummyLines);
}

void kerryCamExposureSave() {
  kerry_cam_exp_cfg_t copy;
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  copy = g_cfg;
  xSemaphoreGive(g_storeMu);
  writeNvs(&copy);
}

void kerryCamExposureActivateStaged() {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (!g_hasStaged) {
    xSemaphoreGive(g_storeMu);
    return;
  }
  g_prevCfg = g_cfg;
  g_cfg = g_staged;
  g_appliedSinceActivate = false;
  xSemaphoreGive(g_storeMu);
}

bool kerryCamExposurePersistStaged() {
  kerry_cam_exp_cfg_t toSave;
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (!g_hasStaged) {
    xSemaphoreGive(g_storeMu);
    return true;
  }
  if (kerryCamExposureCfgEqual(&g_cfg, &g_staged)) {
    toSave = g_cfg;
    g_hasStaged = false;
    xSemaphoreGive(g_storeMu);
    writeNvs(&toSave);
    return true;
  }
  g_hasStaged = false;
  xSemaphoreGive(g_storeMu);
  KERRY_LOGW("exposure", "staged exposure was reverted before persist, NVS unchanged");
  return false;
}

bool kerryCamExposureRollbackStaged() {
  bool reapply = false;
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (!g_hasStaged) {
    xSemaphoreGive(g_storeMu);
    return false;
  }
  reapply = g_appliedSinceActivate;
  g_cfg = g_prevCfg;
  g_hasStaged = false;
  g_appliedSinceActivate = false;
  xSemaphoreGive(g_storeMu);
  return reapply;
}

void kerryCamExposureNoteCaptureOk() {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (g_unverified) {
    g_lastGood = g_cfg;
    g_hasLastGood = true;
    g_unverified = false;
  }
  g_captureFailStreak = 0;
  xSemaphoreGive(g_storeMu);
}

void kerryCamExposureNoteCaptureFail() {
  kerry_cam_exp_cfg_t revertCfg;
  bool scheduleRevert = false;
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (g_captureFailStreak < 255) g_captureFailStreak++;
  if (g_captureFailStreak >= 2 && g_unverified && g_hasLastGood &&
      !kerryCamExposureCfgEqual(&g_cfg, &g_lastGood)) {
    revertCfg = g_lastGood;
    g_cfg = g_lastGood;
    scheduleRevert = true;
    g_revertPending = true;
  }
  xSemaphoreGive(g_storeMu);
  if (scheduleRevert) {
    KERRY_LOGW("exposure", "capture failed twice, scheduling exposure revert");
    writeNvs(&revertCfg);
  }
}

bool kerryCamExposureRevertPending() {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  bool pending = g_revertPending;
  xSemaphoreGive(g_storeMu);
  return pending;
}

void kerryCamExpStoreNoteApplied() {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (g_hasStaged) g_appliedSinceActivate = true;
  xSemaphoreGive(g_storeMu);
}

void kerryCamExpStoreMarkApplied() {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  g_unverified = true;
  g_captureFailStreak = 0;
  xSemaphoreGive(g_storeMu);
}

void kerryCamExpStoreClearRevertPending() {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  g_revertPending = false;
  xSemaphoreGive(g_storeMu);
}
