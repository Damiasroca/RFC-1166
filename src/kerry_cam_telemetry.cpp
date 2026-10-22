#include "kerry_cam_telemetry.h"

#include "kerry_awb.h"
#include "kerry_cam_exposure.h"
#include "kerry_cam_exposure_cfg.h"
#include "kerry_cam_exposure_store.h"
#include "kerry_cam_luma.h"
#include "kerry_cam_ov2640.h"
#include "kerry_log.h"
#include "kerry_shared.h"

#include <stdio.h>

static const char *TAG = "camstat";

static uint32_t s_next_ms = KERRY_CAM_TELEMETRY_INTERVAL_MS;

static const char *awb_mode_label(int mode) {
  switch (mode) {
    case 0:
      return "Auto";
    case 1:
      return "Sunny";
    case 2:
      return "Cloudy";
    case 3:
      return "Office";
    case 4:
      return "Home";
    default:
      if (mode >= KERRY_AWB_PRESET0 && mode <= KERRY_AWB_PRESET_LAST) return "Preset";
      return "?";
  }
}

static const char *gain_ceil_label(uint8_t idx) {
  static const char *names[] = {"2x", "4x", "8x", "16x", "32x", "64x", "128x"};
  if (idx > GAINCEILING_128X) return "?";
  return names[idx];
}

static void log_block(const char *title) {
  KERRY_LOGI(TAG, "---------- %s ----------", title);
}

static bool may_run(uint32_t now_ms) {
  if ((int32_t)(now_ms - s_next_ms) < 0) return false;
  return kerryCamSamplingAllowed();
}

void kerryCamTelemetryService(uint32_t now_ms) {
  if (!may_run(now_ms)) return;
  s_next_ms = now_ms + KERRY_CAM_TELEMETRY_INTERVAL_MS;

  kerry_cam_exp_cfg_t cfg;
  kerryCamExpStoreGet(&cfg);

  kerry_cam_sensor_snapshot_t snap;
  bool haveSnap = kerryCamReadSensorSnapshot(&snap);

  JsonDocument imgDoc;
  kerryImageGet(imgDoc);
  JsonDocument awbDoc;
  kerryAwbFillJson(awbDoc);

  uint16_t pid = haveSnap ? snap.pid : 0;
  const char *sensor = kerryCameraSensorName();

  log_block("Camera settings");
  KERRY_LOGI(TAG, "sensor=%s pid=0x%04X size=%s", sensor, (unsigned)pid,
             imgDoc["framesizeLabel"] | "?");

  char expLine[180];
  snprintf(expLine, sizeof expLine,
           "exposure cfg: AEC=%s val=%u AEC2=%s aeLvl=%d | AGC=%s gain=%u ceil=%s",
           cfg.aecOn ? "on" : "off", (unsigned)cfg.aecValue, cfg.aec2On ? "on" : "off", (int)cfg.aeLevel,
           cfg.agcOn ? "on" : "off", (unsigned)cfg.agcGain,
           cfg.gainCeiling == KERRY_EXP_GAIN_CEIL_SENTINEL ? "default" : gain_ceil_label(cfg.gainCeiling));
  KERRY_LOGI(TAG, "%s", expLine);

  if (haveSnap && snap.haveOv2640Live) {
    const kerry_ov2640_live_t *live = &snap.ov2640;
    // OV2640 GAIN: bits 7..4 each double the gain, bits 3..0 add n/16.
    unsigned coarse = 1;
    if (live->gainReg & 0x10) coarse *= 2;
    if (live->gainReg & 0x20) coarse *= 2;
    if (live->gainReg & 0x40) coarse *= 2;
    if (live->gainReg & 0x80) coarse *= 2;
    unsigned x16 = coarse * (16u + (unsigned)(live->gainReg & 0x0F));
    snprintf(expLine, sizeof expLine,
             "exposure live: AEC=%s val=%u | AGC=%s gain=0x%02X x%u.%04u",
             live->aecOn ? "on" : "off", (unsigned)live->aecValue, live->agcOn ? "on" : "off",
             (unsigned)live->gainReg, x16 / 16u, (x16 % 16u) * 625u);
    KERRY_LOGI(TAG, "%s", expLine);
  }
  KERRY_LOGI(TAG, "exposure est=%ums meas=%ums", (unsigned)kerryCamExposureEstimatedMs(),
             (unsigned)kerryCamExposureMeasuredMs());

  if (kerryOv2640Is(pid)) {
    snprintf(expLine, sizeof expLine, "exposure timing: clkrc=0x%02X dummyLines=%u",
             (unsigned)(cfg.clkrc == KERRY_EXP_CLKRC_SENTINEL ? 0xFF : cfg.clkrc), (unsigned)cfg.dummyLines);
    KERRY_LOGI(TAG, "%s", expLine);
  }

  int awbMode = awbDoc["awb"] | 0;
  JsonArray live = awbDoc["live"].as<JsonArray>();
  snprintf(expLine, sizeof expLine, "white balance: mode=%s (%d) stored=%u/%u/%u",
           awb_mode_label(awbMode), awbMode, (unsigned)(awbDoc["awb_r"] | 0), (unsigned)(awbDoc["awb_g"] | 0),
           (unsigned)(awbDoc["awb_b"] | 0));
  KERRY_LOGI(TAG, "%s", expLine);
  if (!live.isNull() && live.size() >= 3) {
    KERRY_LOGI(TAG, "white balance live gains: R=%d G=%d B=%d", live[0].as<int>(), live[1].as<int>(),
               live[2].as<int>());
  }

  KERRY_LOGI(TAG, "picture: bright=%d contrast=%d sat=%d flipH=%d flipV=%d quality=%d",
             imgDoc["brightness"] | 0, imgDoc["contrast"] | 0, imgDoc["saturation"] | 0, imgDoc["flipH"] | 0,
             imgDoc["flipV"] | 0, haveSnap ? snap.quality : -1);

  uint8_t hwLuma = haveSnap ? snap.hwLuma : 0;
  bool haveHw = haveSnap && snap.haveHwLuma;

  kerry_luma_t frameLuma;
  bool haveLuma = kerryCamSampleLuma(&frameLuma);

  if (haveLuma && haveHw) {
    KERRY_LOGI(TAG, "luma: frame avg=%u min=%u max=%u | sensor avg=%u", (unsigned)frameLuma.avg,
               (unsigned)frameLuma.min, (unsigned)frameLuma.max, (unsigned)hwLuma);
  } else if (haveLuma) {
    KERRY_LOGI(TAG, "luma: frame avg=%u min=%u max=%u", (unsigned)frameLuma.avg, (unsigned)frameLuma.min,
               (unsigned)frameLuma.max);
  } else if (haveHw) {
    KERRY_LOGI(TAG, "luma: sensor avg=%u (frame sample skipped)", (unsigned)hwLuma);
  } else {
    KERRY_LOGW(TAG, "luma: unavailable");
  }

  KERRY_LOGI(TAG, "----------------------------------------");
}
