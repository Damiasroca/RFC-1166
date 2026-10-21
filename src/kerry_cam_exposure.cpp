#include "kerry_cam_exposure.h"

#include "kerry_log.h"
#include "kerry_shared.h"

#include <Preferences.h>
#include <esp_camera.h>

typedef struct {
  bool aecOn;
  uint16_t aecValue;
  bool aec2On;
  int8_t aeLevel;
  bool agcOn;
  uint8_t agcGain;
  uint8_t gainCeiling;
  uint8_t clkrc;
  uint16_t dummyLines;
} kerry_cam_exp_snapshot_t;

static bool g_aecOn = true;
static uint16_t g_aecValue = 600;
static bool g_aec2On = false;
static int8_t g_aeLevel = 0;
static bool g_agcOn = true;
static uint8_t g_agcGain = 15;
static uint8_t g_gainCeiling = KERRY_EXP_GAIN_CEIL_SENTINEL;
static uint8_t g_clkrc = KERRY_EXP_CLKRC_SENTINEL;
static uint16_t g_dummyLines = 0;

static kerry_cam_exp_snapshot_t g_lastGood;
static bool g_hasLastGood = false;
static uint8_t g_captureFailStreak = 0;
static volatile bool g_revertPending = false;

static void snapshotNow(kerry_cam_exp_snapshot_t *out) {
  out->aecOn = g_aecOn;
  out->aecValue = g_aecValue;
  out->aec2On = g_aec2On;
  out->aeLevel = g_aeLevel;
  out->agcOn = g_agcOn;
  out->agcGain = g_agcGain;
  out->gainCeiling = g_gainCeiling;
  out->clkrc = g_clkrc;
  out->dummyLines = g_dummyLines;
}

static void restoreSnapshot(const kerry_cam_exp_snapshot_t *in) {
  g_aecOn = in->aecOn;
  g_aecValue = in->aecValue;
  g_aec2On = in->aec2On;
  g_aeLevel = in->aeLevel;
  g_agcOn = in->agcOn;
  g_agcGain = in->agcGain;
  g_gainCeiling = in->gainCeiling;
  g_clkrc = in->clkrc;
  g_dummyLines = in->dummyLines;
}

static bool isOv2640Pid(uint16_t pid) {
  return pid == OV2640_PID;
}

static int aeLevelMin(uint16_t pid) { return isOv2640Pid(pid) ? -2 : -5; }

static int aeLevelMax(uint16_t pid) { return isOv2640Pid(pid) ? 2 : 5; }

static uint16_t aecValueMax(uint16_t pid) { return isOv2640Pid(pid) ? 1200 : 65535; }

static bool clkrcAllowed(uint8_t v) {
  return v == KERRY_EXP_CLKRC_SENTINEL || v == 0x80 || v == 0xC0 || v == 0xE0;
}

static gainceiling_t gainCeilingEnum(uint8_t idx) {
  if (idx > GAINCEILING_128X) idx = GAINCEILING_128X;
  return (gainceiling_t)idx;
}

static void applyGainCeiling(sensor_t *s, uint16_t pid) {
  if (!s->set_gainceiling) return;
  if (g_gainCeiling != KERRY_EXP_GAIN_CEIL_SENTINEL) {
    s->set_gainceiling(s, gainCeilingEnum(g_gainCeiling));
    return;
  }
  if (isOv2640Pid(pid)) {
    s->set_gainceiling(s, GAINCEILING_16X);
  } else if (pid == OV5640_PID) {
    s->set_gainceiling(s, GAINCEILING_8X);
  }
}

static int applyOv2640Timing(sensor_t *s) {
  if (!s->set_reg) return 0;
  int ret = 0;
  if (g_clkrc != KERRY_EXP_CLKRC_SENTINEL) {
    ret = s->set_reg(s, 0x0111, 0xFF, g_clkrc);
    if (ret) return ret;
  }
  if (g_dummyLines > 0) {
    ret = s->set_reg(s, 0x012D, 0xFF, (int)(g_dummyLines & 0xFF));
    if (ret) return ret;
    ret = s->set_reg(s, 0x012E, 0xFF, (int)((g_dummyLines >> 8) & 0xFF));
  }
  return ret;
}

void kerryCamExposureLoad() {
  Preferences prefs;
  prefs.begin("kerry", true);
  g_aecOn = prefs.getBool("exp_aec", true);
  g_aecValue = prefs.getUShort("exp_aecv", 600);
  g_aec2On = prefs.getBool("exp_aec2", false);
  g_aeLevel = prefs.getChar("exp_ael", 0);
  g_agcOn = prefs.getBool("exp_agc", true);
  g_agcGain = prefs.getUChar("exp_agcg", 15);
  g_gainCeiling = prefs.getUChar("exp_gceil", KERRY_EXP_GAIN_CEIL_SENTINEL);
  g_clkrc = prefs.getUChar("exp_clkrc", KERRY_EXP_CLKRC_SENTINEL);
  g_dummyLines = prefs.getUShort("exp_dummy", 0);
  prefs.end();
  snapshotNow(&g_lastGood);
  g_hasLastGood = true;
  KERRY_LOGI("exposure", "loaded aec=%d agc=%d clkrc=0x%02X dummy=%u", g_aecOn ? 1 : 0, g_agcOn ? 1 : 0,
             (unsigned)g_clkrc, (unsigned)g_dummyLines);
}

void kerryCamExposureSave() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putBool("exp_aec", g_aecOn);
  prefs.putUShort("exp_aecv", g_aecValue);
  prefs.putBool("exp_aec2", g_aec2On);
  prefs.putChar("exp_ael", g_aeLevel);
  prefs.putBool("exp_agc", g_agcOn);
  prefs.putUChar("exp_agcg", g_agcGain);
  prefs.putUChar("exp_gceil", g_gainCeiling);
  prefs.putUChar("exp_clkrc", g_clkrc);
  prefs.putUShort("exp_dummy", g_dummyLines);
  prefs.end();
}

void kerryCamExposureFillJson(JsonDocument &doc) {
  JsonObject exp = doc["exposure"].to<JsonObject>();
  exp["aecOn"] = g_aecOn;
  exp["aecValue"] = g_aecValue;
  exp["aec2On"] = g_aec2On;
  exp["aeLevel"] = g_aeLevel;
  exp["agcOn"] = g_agcOn;
  exp["agcGain"] = g_agcGain;
  exp["gainCeiling"] = g_gainCeiling;
  exp["clkrc"] = g_clkrc;
  exp["dummyLines"] = g_dummyLines;
  exp["estimatedFrameMs"] = kerryCamExposureEstimatedMs();

  JsonObject caps = doc["exposureCaps"].to<JsonObject>();
  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : 0;
  bool known = kerryCameraReady() && s;
  caps["aecMax"] = known ? aecValueMax(pid) : 1200;
  caps["aeLevelMin"] = known ? aeLevelMin(pid) : -5;
  caps["aeLevelMax"] = known ? aeLevelMax(pid) : 5;
  caps["supportsClkrc"] = known && isOv2640Pid(pid);
  caps["supportsDummyLines"] = known && isOv2640Pid(pid);
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
    } opts[] = {{0x80, "Normal (0x80)"}, {0xC0, "Slower (0xC0)"}, {0xE0, "Slowest (0xE0)"}};
    for (auto &o : opts) {
      JsonObject row = clkrcOpts.add<JsonObject>();
      row["value"] = o.v;
      row["label"] = o.label;
    }
  }
}

const char *kerryCamExposureUpdate(JsonObjectConst exp) {
  if (exp.isNull()) return nullptr;

  if (exp["aecOn"].is<bool>()) g_aecOn = exp["aecOn"].as<bool>();
  if (exp["aec2On"].is<bool>()) g_aec2On = exp["aec2On"].as<bool>();
  if (exp["agcOn"].is<bool>()) g_agcOn = exp["agcOn"].as<bool>();

  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : OV2640_PID;

  if (exp["aecValue"].is<int>()) {
    int v = exp["aecValue"].as<int>();
    if (v < 0) return "aecValue out of range";
    uint16_t mx = aecValueMax(pid);
    if ((uint16_t)v > mx) return "aecValue out of range";
    g_aecValue = (uint16_t)v;
  }
  if (exp["aeLevel"].is<int>()) {
    int v = exp["aeLevel"].as<int>();
    if (v < aeLevelMin(pid) || v > aeLevelMax(pid)) return "aeLevel out of range";
    g_aeLevel = (int8_t)v;
  }
  if (exp["agcGain"].is<int>()) {
    int v = exp["agcGain"].as<int>();
    if (v < 0 || v > 30) return "agcGain out of range";
    g_agcGain = (uint8_t)v;
  }
  if (exp["gainCeiling"].is<int>()) {
    int v = exp["gainCeiling"].as<int>();
    if (v != (int)KERRY_EXP_GAIN_CEIL_SENTINEL && (v < 0 || v > (int)GAINCEILING_128X))
      return "gainCeiling out of range";
    g_gainCeiling = (uint8_t)v;
  }
  if (exp["clkrc"].is<int>()) {
    int v = exp["clkrc"].as<int>();
    if (v < 0 || v > 255) return "clkrc out of range";
    if (!clkrcAllowed((uint8_t)v)) return "clkrc not allowed";
    g_clkrc = (uint8_t)v;
  }
  if (exp["dummyLines"].is<int>()) {
    int v = exp["dummyLines"].as<int>();
    if (v < 0 || v > 512) return "dummyLines out of range";
    g_dummyLines = (uint16_t)v;
  }

  if (kerryCamExposureEstimatedMs() > 10000) return "Exposure timing too long; reduce manual exposure or dummy lines";

  kerryCamExposureSave();
  return nullptr;
}

const char *kerryCamExposureApply(bool discardFramesAfter) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !kerryCameraReady()) return "Camera not ready";

  uint16_t pid = s->id.PID;

  if (isOv2640Pid(pid)) {
    if (applyOv2640Timing(s) != 0) {
      KERRY_LOGW("exposure", "OV2640 timing registers rejected");
      return "Sensor rejected clock or dummy-line settings";
    }
  }

  if (g_aecOn) {
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_aec2) s->set_aec2(s, g_aec2On ? 1 : 0);
    if (s->set_ae_level) s->set_ae_level(s, g_aeLevel);
  } else {
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 0);
    if (s->set_aec_value) {
      int v = g_aecValue;
      uint16_t mx = aecValueMax(pid);
      if ((uint16_t)v > mx) v = mx;
      s->set_aec_value(s, v);
    }
  }

  if (g_agcOn) {
    if (s->set_gain_ctrl) s->set_gain_ctrl(s, 1);
    applyGainCeiling(s, pid);
  } else {
    if (s->set_gain_ctrl) s->set_gain_ctrl(s, 0);
    if (s->set_agc_gain) s->set_agc_gain(s, g_agcGain);
  }

  if (pid == OV5640_PID) {
    if (s->set_denoise) s->set_denoise(s, 4);
    if (s->set_quality) s->set_quality(s, 12);
  }

  if (discardFramesAfter) kerryCamDiscardFrames(2);

  snapshotNow(&g_lastGood);
  g_hasLastGood = true;
  g_captureFailStreak = 0;

  KERRY_LOGI("exposure", "applied aec=%d val=%u aec2=%d ae=%d agc=%d gain=%u ceil=0x%02X clkrc=0x%02X dummy=%u",
             g_aecOn ? 1 : 0, (unsigned)g_aecValue, g_aec2On ? 1 : 0, (int)g_aeLevel, g_agcOn ? 1 : 0,
             (unsigned)g_agcGain, (unsigned)g_gainCeiling, (unsigned)g_clkrc, (unsigned)g_dummyLines);
  return nullptr;
}

uint32_t kerryCamExposureEstimatedMs() {
  uint32_t ms = 120;
  if (!g_aecOn) {
    ms = 80 + ((uint32_t)g_aecValue * 2500UL) / 1200UL;
  } else if (g_aec2On) {
    ms = 350;
  }
  if (g_clkrc == 0xC0) ms = (ms * 3) / 2;
  else if (g_clkrc == 0xE0) ms *= 2;
  if (g_dummyLines > 0) ms += (uint32_t)g_dummyLines / 2;
  if (ms > 10000) ms = 10000;
  return ms;
}

bool kerryCamExposureNeedsLenientWdt() { return kerryCamExposureEstimatedMs() > 4000; }

void kerryCamExposureNoteCaptureOk() { g_captureFailStreak = 0; }

void kerryCamExposureNoteCaptureFail() {
  if (g_captureFailStreak < 255) g_captureFailStreak++;
  if (g_captureFailStreak < 2 || !g_hasLastGood) return;
  KERRY_LOGW("exposure", "capture failed twice, scheduling exposure revert");
  restoreSnapshot(&g_lastGood);
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
