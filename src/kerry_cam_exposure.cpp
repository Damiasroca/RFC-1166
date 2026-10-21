#include "kerry_cam_exposure.h"

#include "kerry_cam_exposure_cfg.h"
#include "kerry_cam_exposure_store.h"
#include "kerry_cam_ov2640.h"
#include "kerry_log.h"
#include "kerry_shared.h"

#include <Arduino.h>
#include <esp_camera.h>

static const char *g_lastApplyErr = nullptr;

static void kerryCamExposureSettle(const kerry_cam_exp_cfg_t *cfg);

static gainceiling_t gainCeilingEnum(uint8_t idx) {
  if (idx > GAINCEILING_128X) idx = GAINCEILING_128X;
  return (gainceiling_t)idx;
}

static void applyGainCeiling(sensor_t *s, uint16_t pid, const kerry_cam_exp_cfg_t *cfg) {
  if (!s->set_gainceiling) return;
  if (cfg->gainCeiling != KERRY_EXP_GAIN_CEIL_SENTINEL) {
    s->set_gainceiling(s, gainCeilingEnum(cfg->gainCeiling));
    return;
  }
  if (kerryOv2640Is(pid)) {
    s->set_gainceiling(s, GAINCEILING_16X);
  } else if (pid == OV5640_PID) {
    s->set_gainceiling(s, GAINCEILING_8X);
  }
}

void kerryCamExposureFillJson(JsonDocument &doc) {
  kerry_cam_exp_cfg_t cfg;
  kerryCamExpStoreGet(&cfg);

  JsonObject exp = doc["exposure"].to<JsonObject>();
  exp["aecOn"] = cfg.aecOn;
  exp["aecValue"] = cfg.aecValue;
  exp["aec2On"] = cfg.aec2On;
  exp["aeLevel"] = cfg.aeLevel;
  exp["agcOn"] = cfg.agcOn;
  exp["agcGain"] = cfg.agcGain;
  exp["gainCeiling"] = cfg.gainCeiling;
  exp["clkrc"] = cfg.clkrc;
  exp["dummyLines"] = cfg.dummyLines;
  exp["estimatedFrameMs"] = kerryCamExposureEstimatedMs();
  exp["measuredFrameMs"] = kerryCamExposureMeasuredMs();

  JsonObject caps = doc["exposureCaps"].to<JsonObject>();
  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : 0;
  bool known = kerryCameraReady() && s;
  framesize_t fs = s ? s->status.framesize : FRAMESIZE_SVGA;
  caps["aecMax"] = known ? kerryCamExposureAecValueMax(&cfg, pid, fs) : 1200;
  if (known && kerryOv2640Is(pid)) {
    caps["aecBaseLines"] = kerryOv2640BaseFrameLines(fs);
    JsonObject bySize = caps["aecBaseLinesBySize"].to<JsonObject>();
    const framesize_t allowed[] = {FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XGA, FRAMESIZE_HD,
                                 FRAMESIZE_FHD,  FRAMESIZE_SXGA, FRAMESIZE_UXGA, FRAMESIZE_QXGA};
    for (framesize_t size : allowed) {
      if (!kerryImageFramesizeListed(size)) continue;
      framesize_t effective = kerryFrameSizeEffective(size);
      bySize[String((int)size)] = kerryOv2640BaseFrameLines(effective);
    }
  }
  caps["aeLevelMin"] = known ? kerryCamExposureAeLevelMin(pid) : -5;
  caps["aeLevelMax"] = known ? kerryCamExposureAeLevelMax(pid) : 5;
  caps["supportsClkrc"] = known && kerryOv2640Is(pid);
  caps["supportsDummyLines"] = known && kerryOv2640Is(pid);
  caps["agcGainMax"] = 30;
  caps["gainCeilingMax"] = (int)GAINCEILING_128X;
  int gainDefault = -1;
  if (known) {
    if (kerryOv2640Is(pid)) gainDefault = (int)GAINCEILING_16X;
    else if (pid == OV5640_PID) gainDefault = (int)GAINCEILING_8X;
  }
  caps["gainCeilingDefault"] = gainDefault;

  JsonArray clkrcOpts = caps["clkrcOptions"].to<JsonArray>();
  if (caps["supportsClkrc"].as<bool>()) {
    JsonObject d = clkrcOpts.add<JsonObject>();
    d["value"] = KERRY_EXP_CLKRC_SENTINEL;
    d["label"] = "Driver default";
    const struct {
      uint8_t v;
      const char *label;
    } opts[] = {{0x01, "÷2"}, {0x03, "÷4"}, {0x07, "÷8"}, {0x0F, "÷16"}};
    for (auto &o : opts) {
      JsonObject row = clkrcOpts.add<JsonObject>();
      row["value"] = o.v;
      row["label"] = o.label;
    }
  }
}

const char *kerryCamExposureStage(JsonObjectConst exp, framesize_t targetFs) {
  if (exp.isNull()) return nullptr;

  kerry_cam_exp_cfg_t staged;
  kerryCamExpStoreGet(&staged);
  const char *err = nullptr;
  if (exp["aecOn"].is<bool>()) staged.aecOn = exp["aecOn"].as<bool>();
  if (exp["aec2On"].is<bool>()) staged.aec2On = exp["aec2On"].as<bool>();
  if (exp["agcOn"].is<bool>()) staged.agcOn = exp["agcOn"].as<bool>();

  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : OV2640_PID;

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

  if (!err) err = kerryCamExposureCfgValidate(&staged, pid, targetFs);
  if (err) return err;
  kerryCamExpStoreSetStaged(&staged);
  return nullptr;
}

const char *kerryCamExposureApply(bool discardFramesAfter) {
  kerry_cam_exp_cfg_t cfg;
  kerryCamExpStoreGet(&cfg);

  sensor_t *s = esp_camera_sensor_get();
  if (!s || !kerryCameraReady()) {
    g_lastApplyErr = "Camera not ready";
    return g_lastApplyErr;
  }

  uint16_t pid = s->id.PID;

  kerryCamExpStoreNoteApplied();

  if (kerryOv2640Is(pid)) {
    const char *timingErr = kerryOv2640ApplyTiming(s, cfg.clkrc, cfg.dummyLines);
    if (timingErr) {
      KERRY_LOGW("exposure", "%s", timingErr);
      g_lastApplyErr = timingErr;
      return timingErr;
    }
  }

  if (cfg.aecOn) {
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_aec2) s->set_aec2(s, cfg.aec2On ? 1 : 0);
    if (s->set_ae_level) s->set_ae_level(s, cfg.aeLevel);
  } else {
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 0);
    int v = cfg.aecValue;
    uint16_t mx = kerryCamExposureAecValueMax(&cfg, pid, s->status.framesize);
    if (v > (int)mx) v = (int)mx;
    if (kerryOv2640Is(pid)) {
      if (kerryOv2640SetAecValue(s, v) != 0) {
        KERRY_LOGW("exposure", "OV2640 manual exposure rejected");
        g_lastApplyErr = "Sensor rejected manual exposure";
        return g_lastApplyErr;
      }
      s->status.aec_value = (uint16_t)v;
    } else if (s->set_aec_value) {
      s->set_aec_value(s, v);
    }
  }

  if (cfg.agcOn) {
    if (s->set_gain_ctrl) s->set_gain_ctrl(s, 1);
    applyGainCeiling(s, pid, &cfg);
  } else {
    if (s->set_gain_ctrl) s->set_gain_ctrl(s, 0);
    if (s->set_agc_gain) s->set_agc_gain(s, cfg.agcGain);
  }

  if (pid == OV5640_PID) {
    if (s->set_denoise) s->set_denoise(s, 4);
    if (s->set_quality) s->set_quality(s, 12);
  }

  if (discardFramesAfter) kerryCamExposureSettle(&cfg);

  kerryCamExpStoreMarkApplied();

  KERRY_LOGI("exposure", "applied aec=%d val=%u aec2=%d ae=%d agc=%d gain=%u ceil=0x%02X clkrc=0x%02X dummy=%u",
             cfg.aecOn ? 1 : 0, (unsigned)cfg.aecValue, cfg.aec2On ? 1 : 0, (int)cfg.aeLevel, cfg.agcOn ? 1 : 0,
             (unsigned)cfg.agcGain,              (unsigned)cfg.gainCeiling, (unsigned)cfg.clkrc, (unsigned)cfg.dummyLines);
  g_lastApplyErr = nullptr;
  return nullptr;
}

const char *kerryCamExposureLastApplyError() { return g_lastApplyErr; }

uint32_t kerryCamExposureEstimatedMs() {
  kerry_cam_exp_cfg_t cfg;
  kerryCamExpStoreGet(&cfg);
  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : OV2640_PID;
  framesize_t fs = s ? s->status.framesize : FRAMESIZE_SVGA;
  return kerryCamExposureEstimateMs(&cfg, pid, fs);
}

uint32_t kerryCamExposureFrameMs() {
  uint32_t measured = kerryCamExposureMeasuredMs();
  if (measured > 0) return measured;
  return kerryCamExposureEstimatedMs();
}

static void kerryCamExposureSettle(const kerry_cam_exp_cfg_t *cfg) {
  kerryWdtLenient();
  bool manual = !cfg->aecOn && !cfg->agcOn;
  uint32_t timeCap = kerryCamExposureBudgetMs() * 3UL;
  const int frameCap = 10;
  int minFrames = manual ? 2 : 3;
  uint32_t minMs = manual ? 0 : 1500;
  uint32_t started = millis();
  uint32_t freshTs[10];
  int fresh = 0;
  int attempts = 0;
  kerryCamExposureNoteMeasured(0, cfg, 0, FRAMESIZE_SVGA);
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
  kerryCamExposureNoteMeasured(newer - older, cfg, pid, fs);
  KERRY_LOGD("exposure", "measured frame period %ums", (unsigned)(newer - older));
}

void kerryCamExposureServiceRevert() {
  if (!kerryCamExposureRevertPending() || !kerryCameraReady()) return;
  kerryCamExpStoreClearRevertPending();
  const char *err = kerryCamExposureApply(true);
  if (err) KERRY_LOGW("exposure", "revert apply failed: %s", err);
  else KERRY_LOGI("exposure", "reverted to last good settings");
}
