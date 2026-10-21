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
static SemaphoreHandle_t g_storeMu = nullptr;

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
  prefs.end();

  uint8_t storedClk = loaded.clkrc;
  uint8_t mappedClk = storedClk;
  bool migrateClk = true;
  if (storedClk == 0x81) mappedClk = KERRY_EXP_CLKRC_SENTINEL;
  else if (storedClk == 0x83) mappedClk = 0x01;
  else if (storedClk == 0x87) mappedClk = 0x03;
  else if (storedClk == 0x8F) mappedClk = 0x07;
  else migrateClk = false;

  kerry_cam_exp_cfg_t toSave = loaded;
  bool needSave = false;
  if (migrateClk) {
    KERRY_LOGW("exposure", "stored clkrc 0x%02X migrated to 0x%02X", (unsigned)storedClk, (unsigned)mappedClk);
    toSave.clkrc = mappedClk;
    needSave = true;
  } else if (!kerryCamExposureClkrcAllowed(loaded.clkrc)) {
    KERRY_LOGW("exposure", "stored clkrc 0x%02X is not valid, using driver default", (unsigned)loaded.clkrc);
    toSave.clkrc = KERRY_EXP_CLKRC_SENTINEL;
    needSave = true;
  }

  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  g_cfg = needSave ? toSave : loaded;
  g_lastGood = kerryCamExposureDefaultCfg();
  g_hasLastGood = true;
  g_unverified = false;
  xSemaphoreGive(g_storeMu);

  if (needSave) writeNvs(&toSave);

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
  xSemaphoreGive(g_storeMu);
}

bool kerryCamExposurePersistStaged() {
  kerry_cam_exp_cfg_t toSave;
  bool doSave = false;
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (!g_hasStaged) {
    xSemaphoreGive(g_storeMu);
    return true;
  }
  if (kerryCamExposureCfgEqual(&g_cfg, &g_staged)) {
    toSave = g_cfg;
    g_hasStaged = false;
    doSave = true;
    xSemaphoreGive(g_storeMu);
    writeNvs(&toSave);
    return true;
  }
  g_hasStaged = false;
  xSemaphoreGive(g_storeMu);
  KERRY_LOGW("exposure", "staged exposure was reverted before persist, NVS unchanged");
  return false;
}

void kerryCamExposureRollbackStaged() {
  xSemaphoreTake(g_storeMu, portMAX_DELAY);
  if (!g_hasStaged) {
    xSemaphoreGive(g_storeMu);
    return;
  }
  g_cfg = g_prevCfg;
  g_hasStaged = false;
  xSemaphoreGive(g_storeMu);
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
