#include "kerry_cam_exposure_store.h"

#include "kerry_cam_exposure_cfg.h"
#include "kerry_log.h"

#include <Arduino.h>
#include <Preferences.h>

static kerry_cam_exp_cfg_t g_cfg = kerryCamExposureDefaultCfg();
static kerry_cam_exp_cfg_t g_staged = kerryCamExposureDefaultCfg();
static kerry_cam_exp_cfg_t g_prevCfg = kerryCamExposureDefaultCfg();
static bool g_hasStaged = false;
static kerry_cam_exp_cfg_t g_lastGood = kerryCamExposureDefaultCfg();
static bool g_hasLastGood = false;
static bool g_unverified = false;
static uint8_t g_captureFailStreak = 0;
static volatile bool g_revertPending = false;

void kerryCamExpStoreGet(kerry_cam_exp_cfg_t *out) { *out = g_cfg; }

void kerryCamExpStoreSetStaged(const kerry_cam_exp_cfg_t *cfg) {
  g_staged = *cfg;
  g_hasStaged = true;
}

void kerryCamExposureLoad() {
  Preferences prefs;
  prefs.begin("kerry", true);
  g_cfg.aecOn = prefs.getBool("exp_aec", true);
  g_cfg.aecValue = prefs.getUShort("exp_aecv", 600);
  g_cfg.aec2On = prefs.getBool("exp_aec2", false);
  g_cfg.aeLevel = prefs.getChar("exp_ael", 0);
  g_cfg.agcOn = prefs.getBool("exp_agc", true);
  g_cfg.agcGain = prefs.getUChar("exp_agcg", 15);
  g_cfg.gainCeiling = prefs.getUChar("exp_gceil", KERRY_EXP_GAIN_CEIL_SENTINEL);
  g_cfg.clkrc = prefs.getUChar("exp_clkrc", KERRY_EXP_CLKRC_SENTINEL);
  g_cfg.dummyLines = prefs.getUShort("exp_dummy", 0);
  prefs.end();
  uint8_t storedClk = g_cfg.clkrc;
  uint8_t mappedClk = storedClk;
  bool migrateClk = true;
  if (storedClk == 0x81) mappedClk = KERRY_EXP_CLKRC_SENTINEL;
  else if (storedClk == 0x83) mappedClk = 0x01;
  else if (storedClk == 0x87) mappedClk = 0x03;
  else if (storedClk == 0x8F) mappedClk = 0x07;
  else migrateClk = false;
  if (migrateClk) {
    KERRY_LOGW("exposure", "stored clkrc 0x%02X migrated to 0x%02X", (unsigned)storedClk, (unsigned)mappedClk);
    g_cfg.clkrc = mappedClk;
    kerryCamExposureSave();
  } else if (!kerryCamExposureClkrcAllowed(g_cfg.clkrc)) {
    KERRY_LOGW("exposure", "stored clkrc 0x%02X is not valid, using driver default", (unsigned)g_cfg.clkrc);
    g_cfg.clkrc = KERRY_EXP_CLKRC_SENTINEL;
    kerryCamExposureSave();
  }
  g_lastGood = kerryCamExposureDefaultCfg();
  g_hasLastGood = true;
  g_unverified = false;
  KERRY_LOGI("exposure", "loaded aec=%d agc=%d clkrc=0x%02X dummy=%u", g_cfg.aecOn ? 1 : 0, g_cfg.agcOn ? 1 : 0,
             (unsigned)g_cfg.clkrc, (unsigned)g_cfg.dummyLines);
}

void kerryCamExposureSave() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putBool("exp_aec", g_cfg.aecOn);
  prefs.putUShort("exp_aecv", g_cfg.aecValue);
  prefs.putBool("exp_aec2", g_cfg.aec2On);
  prefs.putChar("exp_ael", g_cfg.aeLevel);
  prefs.putBool("exp_agc", g_cfg.agcOn);
  prefs.putUChar("exp_agcg", g_cfg.agcGain);
  prefs.putUChar("exp_gceil", g_cfg.gainCeiling);
  prefs.putUChar("exp_clkrc", g_cfg.clkrc);
  prefs.putUShort("exp_dummy", g_cfg.dummyLines);
  prefs.end();
}

void kerryCamExposureActivateStaged() {
  if (!g_hasStaged) return;
  g_prevCfg = g_cfg;
  g_cfg = g_staged;
}

void kerryCamExposurePersistStaged() {
  if (!g_hasStaged) return;
  kerryCamExposureSave();
  g_hasStaged = false;
}

void kerryCamExposureRollbackStaged() {
  if (!g_hasStaged) return;
  g_cfg = g_prevCfg;
  g_hasStaged = false;
}

void kerryCamExposureNoteCaptureOk() {
  if (g_unverified) {
    g_lastGood = g_cfg;
    g_hasLastGood = true;
    g_unverified = false;
  }
  g_captureFailStreak = 0;
}

void kerryCamExposureNoteCaptureFail() {
  if (g_captureFailStreak < 255) g_captureFailStreak++;
  if (g_captureFailStreak < 2 || !g_unverified || !g_hasLastGood) return;
  if (kerryCamExposureCfgEqual(&g_cfg, &g_lastGood)) return;
  KERRY_LOGW("exposure", "capture failed twice, scheduling exposure revert");
  g_cfg = g_lastGood;
  kerryCamExposureSave();
  g_revertPending = true;
}

bool kerryCamExposureRevertPending() { return g_revertPending; }

void kerryCamExpStoreMarkApplied() {
  g_unverified = true;
  g_captureFailStreak = 0;
}

void kerryCamExpStoreClearRevertPending() { g_revertPending = false; }
