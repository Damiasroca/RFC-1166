#include "kerry_cam_exposure.h"

#include "kerry_cam_exposure_cfg.h"
#include "kerry_cam_ov2640.h"
#include "kerry_log.h"
#include "kerry_shared.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_camera.h>

static kerry_cam_exp_cfg_t g_cfg = kerryCamExposureDefaultCfg();
static kerry_cam_exp_cfg_t g_lastGood = kerryCamExposureDefaultCfg();
static bool g_hasLastGood = false;
static bool g_unverified = false;
static uint8_t g_captureFailStreak = 0;
static volatile bool g_revertPending = false;

static void kerryCamExposureSettle();

static gainceiling_t gainCeilingEnum(uint8_t idx) {
  if (idx > GAINCEILING_128X) idx = GAINCEILING_128X;
  return (gainceiling_t)idx;
}

static void applyGainCeiling(sensor_t *s, uint16_t pid) {
  if (!s->set_gainceiling) return;
  if (g_cfg.gainCeiling != KERRY_EXP_GAIN_CEIL_SENTINEL) {
    s->set_gainceiling(s, gainCeilingEnum(g_cfg.gainCeiling));
    return;
  }
  if (kerryOv2640Is(pid)) {
    s->set_gainceiling(s, GAINCEILING_16X);
  } else if (pid == OV5640_PID) {
    s->set_gainceiling(s, GAINCEILING_8X);
  }
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
  if (!kerryCamExposureClkrcAllowed(g_cfg.clkrc)) {
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

void kerryCamExposureFillJson(JsonDocument &doc) {
  JsonObject exp = doc["exposure"].to<JsonObject>();
  exp["aecOn"] = g_cfg.aecOn;
  exp["aecValue"] = g_cfg.aecValue;
  exp["aec2On"] = g_cfg.aec2On;
  exp["aeLevel"] = g_cfg.aeLevel;
  exp["agcOn"] = g_cfg.agcOn;
  exp["agcGain"] = g_cfg.agcGain;
  exp["gainCeiling"] = g_cfg.gainCeiling;
  exp["clkrc"] = g_cfg.clkrc;
  exp["dummyLines"] = g_cfg.dummyLines;
  exp["estimatedFrameMs"] = kerryCamExposureEstimatedMs();
  exp["measuredFrameMs"] = kerryCamExposureMeasuredMs();

  JsonObject caps = doc["exposureCaps"].to<JsonObject>();
  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : 0;
  bool known = kerryCameraReady() && s;
  framesize_t fs = s ? s->status.framesize : FRAMESIZE_SVGA;
  caps["aecMax"] = known ? kerryCamExposureAecValueMax(&g_cfg, pid, fs) : 1200;
  if (known && kerryOv2640Is(pid)) caps["aecBaseLines"] = kerryOv2640BaseFrameLines(fs);
  caps["aeLevelMin"] = known ? kerryCamExposureAeLevelMin(pid) : -5;
  caps["aeLevelMax"] = known ? kerryCamExposureAeLevelMax(pid) : 5;
  caps["supportsClkrc"] = known && kerryOv2640Is(pid);
  caps["supportsDummyLines"] = known && kerryOv2640Is(pid);
  caps["agcGainMax"] = 30;
  caps["gainCeilingMax"] = (int)GAINCEILING_128X;

  JsonArray clkrcOpts = caps["clkrcOptions"].to<JsonArray>();
  if (caps["supportsClkrc"].as<bool>()) {
    JsonObject d = clkrcOpts.add<JsonObject>();
    d["value"] = KERRY_EXP_CLKRC_SENTINEL;
    d["label"] = "Driver default";
    const struct {
      uint8_t v;
      const char *label;
    } opts[] = {{0x81, "÷2"}, {0x83, "÷4"}, {0x87, "÷8"}, {0x8F, "÷16"}};
    for (auto &o : opts) {
      JsonObject row = clkrcOpts.add<JsonObject>();
      row["value"] = o.v;
      row["label"] = o.label;
    }
  }
}

const char *kerryCamExposureUpdate(JsonObjectConst exp) {
  if (exp.isNull()) return nullptr;

  kerry_cam_exp_cfg_t staged = g_cfg;
  const char *err = nullptr;
  if (exp["aecOn"].is<bool>()) staged.aecOn = exp["aecOn"].as<bool>();
  if (exp["aec2On"].is<bool>()) staged.aec2On = exp["aec2On"].as<bool>();
  if (exp["agcOn"].is<bool>()) staged.agcOn = exp["agcOn"].as<bool>();

  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : OV2640_PID;
  framesize_t fs = s ? s->status.framesize : FRAMESIZE_SVGA;

  if (exp["aecValue"].is<int>()) {
    int v = exp["aecValue"].as<int>();
    if (v < 0 || v > 65535) {
      if (!err) err = "aecValue out of range";
    } else {
      staged.aecValue = (uint16_t)v;
    }
  }
  if (exp["aeLevel"].is<int>()) {
    int v = exp["aeLevel"].as<int>();
    if (v < -128 || v > 127) {
      if (!err) err = "aeLevel out of range";
    } else {
      staged.aeLevel = (int8_t)v;
    }
  }
  if (exp["agcGain"].is<int>()) {
    int v = exp["agcGain"].as<int>();
    if (v < 0 || v > 255) {
      if (!err) err = "agcGain out of range";
    } else {
      staged.agcGain = (uint8_t)v;
    }
  }
  if (exp["gainCeiling"].is<int>()) {
    int v = exp["gainCeiling"].as<int>();
    if (v < 0 || v > 255) {
      if (!err) err = "gainCeiling out of range";
    } else {
      staged.gainCeiling = (uint8_t)v;
    }
  }
  if (exp["clkrc"].is<int>()) {
    int v = exp["clkrc"].as<int>();
    if (v < 0 || v > 255) {
      if (!err) err = "clkrc out of range";
    } else {
      staged.clkrc = (uint8_t)v;
    }
  }
  if (exp["dummyLines"].is<int>()) {
    int v = exp["dummyLines"].as<int>();
    if (v < 0 || v > 65535) {
      if (!err) err = "dummyLines out of range";
    } else {
      staged.dummyLines = (uint16_t)v;
    }
  }

  if (!err) err = kerryCamExposureCfgValidate(&staged, pid, fs);
  if (err) return err;
  g_cfg = staged;
  kerryCamExposureSave();
  return nullptr;
}

const char *kerryCamExposureApply(bool discardFramesAfter) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !kerryCameraReady()) return "Camera not ready";

  uint16_t pid = s->id.PID;

  if (kerryOv2640Is(pid)) {
    const char *timingErr = kerryOv2640ApplyTiming(s, g_cfg.clkrc, g_cfg.dummyLines);
    if (timingErr) {
      KERRY_LOGW("exposure", "%s", timingErr);
      return timingErr;
    }
  }

  if (g_cfg.aecOn) {
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_aec2) s->set_aec2(s, g_cfg.aec2On ? 1 : 0);
    if (s->set_ae_level) s->set_ae_level(s, g_cfg.aeLevel);
  } else {
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 0);
    int v = g_cfg.aecValue;
    uint16_t mx = kerryCamExposureAecValueMax(&g_cfg, pid, s->status.framesize);
    if (v > (int)mx) v = (int)mx;
    if (kerryOv2640Is(pid)) {
      if (kerryOv2640SetAecValue(s, v) != 0) {
        KERRY_LOGW("exposure", "OV2640 manual exposure rejected");
        return "Sensor rejected manual exposure";
      }
      s->status.aec_value = (uint16_t)v;
    } else if (s->set_aec_value) {
      s->set_aec_value(s, v);
    }
  }

  if (g_cfg.agcOn) {
    if (s->set_gain_ctrl) s->set_gain_ctrl(s, 1);
    applyGainCeiling(s, pid);
  } else {
    if (s->set_gain_ctrl) s->set_gain_ctrl(s, 0);
    if (s->set_agc_gain) s->set_agc_gain(s, g_cfg.agcGain);
  }

  if (pid == OV5640_PID) {
    if (s->set_denoise) s->set_denoise(s, 4);
    if (s->set_quality) s->set_quality(s, 12);
  }

  if (discardFramesAfter) kerryCamExposureSettle();

  g_unverified = true;
  g_captureFailStreak = 0;

  KERRY_LOGI("exposure", "applied aec=%d val=%u aec2=%d ae=%d agc=%d gain=%u ceil=0x%02X clkrc=0x%02X dummy=%u",
             g_cfg.aecOn ? 1 : 0, (unsigned)g_cfg.aecValue, g_cfg.aec2On ? 1 : 0, (int)g_cfg.aeLevel,
             g_cfg.agcOn ? 1 : 0, (unsigned)g_cfg.agcGain, (unsigned)g_cfg.gainCeiling, (unsigned)g_cfg.clkrc,
             (unsigned)g_cfg.dummyLines);
  return nullptr;
}

uint32_t kerryCamExposureEstimatedMs() {
  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : OV2640_PID;
  framesize_t fs = s ? s->status.framesize : FRAMESIZE_SVGA;
  return kerryCamExposureEstimateMs(&g_cfg, pid, fs);
}

uint32_t kerryCamExposureFrameMs() {
  uint32_t measured = kerryCamExposureMeasuredMs();
  if (measured > 0) return measured;
  return kerryCamExposureEstimatedMs();
}

bool kerryCamExposureNeedsLenientWdt() { return kerryCamExposureFrameMs() > 4000; }

static void kerryCamExposureSettle() {
  kerryWdtLenient();
  bool manual = !g_cfg.aecOn && !g_cfg.agcOn;
  uint32_t timeCap = kerryCamExposureBudgetMs() * 3UL;
  const int frameCap = 10;
  int minFrames = manual ? 2 : 3;
  uint32_t minMs = manual ? 0 : 1500;
  uint32_t started = millis();
  uint32_t freshTs[10];
  int fresh = 0;
  int attempts = 0;
  kerryCamExposureNoteMeasured(0, &g_cfg, 0, FRAMESIZE_SVGA);
  while (fresh < frameCap && attempts < frameCap && (uint32_t)(millis() - started) < timeCap) {
    uint32_t ts = 0;
    int n = kerryCamDiscardFrames(1, &ts, 1);
    attempts++;
    if (n <= 0) break;
    if ((int32_t)(started - ts) > 50) continue;
    freshTs[fresh++] = ts;
    if (fresh >= minFrames && (uint32_t)(millis() - started) >= minMs) break;
  }
  kerryWdtRestore();
  if (fresh < 2) return;
  uint32_t newer = freshTs[fresh - 1];
  uint32_t older = freshTs[fresh - 2];
  if (newer <= older) return;
  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : 0;
  framesize_t fs = s ? s->status.framesize : FRAMESIZE_SVGA;
  kerryCamExposureNoteMeasured(newer - older, &g_cfg, pid, fs);
  KERRY_LOGD("exposure", "measured frame period %ums", (unsigned)(newer - older));
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

void kerryCamExposureServiceRevert() {
  if (!g_revertPending || !kerryCameraReady()) return;
  g_revertPending = false;
  const char *err = kerryCamExposureApply(true);
  if (err) KERRY_LOGW("exposure", "revert apply failed: %s", err);
  else KERRY_LOGI("exposure", "reverted to last good settings");
}
