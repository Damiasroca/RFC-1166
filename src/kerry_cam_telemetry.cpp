#include "kerry_cam_telemetry.h"

#include "kerry_awb.h"
#include "kerry_cam_exposure.h"
#include "kerry_cam_exposure_cfg.h"
#include "kerry_cam_exposure_store.h"
#include "kerry_cam_ov2640.h"
#include "kerry_log.h"
#include "kerry_shared.h"

#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <rom/tjpgd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "camstat";

static uint32_t s_next_ms = KERRY_CAM_TELEMETRY_INTERVAL_MS;

typedef struct {
  const uint8_t *jpeg;
  size_t len;
  size_t pos;
  uint64_t sum;
  uint32_t count;
  uint8_t min_y;
  uint8_t max_y;
} luma_dec_t;

static UINT luma_in_cb(JDEC *jd, BYTE *buf, UINT n) {
  luma_dec_t *d = (luma_dec_t *)jd->device;
  if (d->pos >= d->len || n == 0) return 0;
  if (n > d->len - d->pos) n = (UINT)(d->len - d->pos);
  if (buf) memcpy(buf, d->jpeg + d->pos, n);
  d->pos += n;
  return n;
}

static inline uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b) {
  return (uint8_t)((77u * (uint32_t)r + 150u * (uint32_t)g + 29u * (uint32_t)b) >> 8);
}

static UINT luma_out_cb(JDEC *jd, void *bitmap, JRECT *rect) {
  luma_dec_t *d = (luma_dec_t *)jd->device;
  if (!bitmap || !rect) return 0;
  if (rect->left > rect->right || rect->top > rect->bottom) return 0;
  int rw = rect->right - rect->left + 1;
  int rh = rect->bottom - rect->top + 1;
  const uint8_t *src = (const uint8_t *)bitmap;
  for (int row = 0; row < rh; row++) {
    const uint8_t *px = src + (size_t)row * (size_t)rw * 3;
    for (int col = 0; col < rw; col++) {
      uint8_t y = rgb_to_y(px[0], px[1], px[2]);
      d->sum += y;
      d->count++;
      if (d->count == 1) {
        d->min_y = y;
        d->max_y = y;
      } else {
        if (y < d->min_y) d->min_y = y;
        if (y > d->max_y) d->max_y = y;
      }
      px += 3;
    }
  }
  return 1;
}

static bool luma_from_jpeg(const uint8_t *jpeg, size_t len, uint8_t *avg, uint8_t *min_y, uint8_t *max_y) {
  if (!jpeg || len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;
  luma_dec_t d = {};
  d.jpeg = jpeg;
  d.len = len;
  JDEC jd;
  uint8_t *work = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!work) work = (uint8_t *)malloc(4096);
  if (!work) return false;
  JRESULT jr = jd_prepare(&jd, luma_in_cb, work, 4096, &d);
  if (jr != JDR_OK) {
    free(work);
    return false;
  }
  if (jd.width < 8 || jd.height < 8) {
    free(work);
    return false;
  }
  BYTE scale = 2;
  if (jd.width > 1280 || jd.height > 960) scale = 3;
  jr = jd_decomp(&jd, luma_out_cb, scale);
  free(work);
  if (jr != JDR_OK || d.count == 0) return false;
  *avg = (uint8_t)(d.sum / d.count);
  *min_y = d.min_y;
  *max_y = d.max_y;
  return true;
}

static bool sensor_hw_luma(sensor_t *s, uint8_t *out) {
  if (!s || !s->get_reg || !out) return false;
  uint16_t pid = s->id.PID;
  if (pid != OV5640_PID && pid != OV3660_PID) return false;
  int lo = s->get_reg(s, 0x5688, 0xFF);
  int hi = s->get_reg(s, 0x5689, 0xFF);
  if (lo < 0 || hi < 0) return false;
  uint16_t raw = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
  *out = (uint8_t)(raw >> 8);
  return true;
}

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

static void refresh_sensor_status(sensor_t *s) {
  if (s && s->init_status) s->init_status(s);
}

static void log_block(const char *title) {
  KERRY_LOGI(TAG, "---------- %s ----------", title);
}

static bool may_run(uint32_t now_ms) {
  if (now_ms < s_next_ms) return false;
  if (!kerryCameraReady()) return false;
  if (kerryStreamActive() || kerryCamGrabActive()) return false;
  const char *blocked = nullptr;
  if (kerryCaptureBlocked(&blocked)) return false;
  if (kerryEcoNetBusy() || kerryUploadInProgress()) return false;
  return true;
}

void kerryCamTelemetryService(uint32_t now_ms) {
  if (!may_run(now_ms)) return;
  s_next_ms = now_ms + KERRY_CAM_TELEMETRY_INTERVAL_MS;

  kerry_cam_exp_cfg_t cfg;
  kerryCamExpStoreGet(&cfg);

  sensor_t *s = esp_camera_sensor_get();
  refresh_sensor_status(s);

  JsonDocument imgDoc;
  kerryImageGet(imgDoc);
  JsonDocument awbDoc;
  kerryAwbFillJson(awbDoc);

  uint16_t pid = s ? s->id.PID : 0;
  const char *sensor = kerryCameraSensorName();
  int liveAec = s ? (int)s->status.aec_value : -1;
  int liveAgc = s ? (int)s->status.agc_gain : -1;
  int liveAecOn = s ? (int)s->status.aec : -1;
  int liveAgcOn = s ? (int)s->status.agc : -1;

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

  if (liveAec >= 0) {
    snprintf(expLine, sizeof expLine,
             "exposure live: AEC=%s val=%d | AGC=%s gain=%d est=%ums meas=%ums",
             liveAecOn ? "on" : "off", liveAec, liveAgcOn ? "on" : "off", liveAgc,
             (unsigned)kerryCamExposureEstimatedMs(), (unsigned)kerryCamExposureMeasuredMs());
    KERRY_LOGI(TAG, "%s", expLine);
  }

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
             imgDoc["flipV"] | 0, s ? (int)s->status.quality : -1);

  uint8_t hwLuma = 0;
  bool haveHw = sensor_hw_luma(s, &hwLuma);

  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  bool haveFrame = kerryCamTelemetryGrabCopy(&jpeg, &jpegLen);
  uint8_t avg = 0, minY = 0, maxY = 0;
  bool haveLuma = haveFrame && luma_from_jpeg(jpeg, jpegLen, &avg, &minY, &maxY);
  if (jpeg) kerryCamTelemetryFreeCopy(jpeg);

  if (haveLuma && haveHw) {
    KERRY_LOGI(TAG, "luma: frame avg=%u min=%u max=%u | sensor avg=%u", (unsigned)avg, (unsigned)minY,
               (unsigned)maxY, (unsigned)hwLuma);
  } else if (haveLuma) {
    KERRY_LOGI(TAG, "luma: frame avg=%u min=%u max=%u", (unsigned)avg, (unsigned)minY, (unsigned)maxY);
  } else if (haveHw) {
    KERRY_LOGI(TAG, "luma: sensor avg=%u (frame sample skipped)", (unsigned)hwLuma);
  } else {
    KERRY_LOGW(TAG, "luma: unavailable");
  }

  KERRY_LOGI(TAG, "----------------------------------------");
}
