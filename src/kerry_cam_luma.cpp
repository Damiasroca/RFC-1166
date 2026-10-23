#include "kerry_cam_luma.h"

#include "kerry_shared.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <rom/tjpgd.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const uint8_t *jpeg;
  size_t len;
  size_t pos;
  uint64_t sum;
  uint64_t linSum;
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

static portMUX_TYPE s_lutMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_lutReady = false;
static uint16_t s_lut[256];

static void ensureLinLut(void) {
  if (s_lutReady) return;
  uint16_t local[256];
  for (int i = 0; i < 256; i++) {
    float v = 65535.0f * powf((float)i / 255.0f, 2.2f);
    long r = lroundf(v);
    if (r < 0) r = 0;
    if (r > 65535) r = 65535;
    local[i] = (uint16_t)r;
  }
  portENTER_CRITICAL(&s_lutMux);
  if (!s_lutReady) {
    memcpy(s_lut, local, sizeof(s_lut));
    s_lutReady = true;
  }
  portEXIT_CRITICAL(&s_lutMux);
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
      d->linSum += s_lut[y];
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

bool kerryLumaFromJpeg(const uint8_t *jpeg, size_t len, kerry_luma_t *out) {
  if (!jpeg || !out || len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;
  ensureLinLut();
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
  out->avg = (uint8_t)(d.sum / d.count);
  out->min = d.min_y;
  out->max = d.max_y;
  out->linMean = (uint16_t)(d.linSum / d.count);
  return true;
}

bool kerryCamSampleLuma(kerry_luma_t *out) {
  if (!out) return false;
  uint8_t *jpeg = nullptr;
  size_t len = 0;
  // Grab plus a full-frame decode runs on the loop task; keep both under the lenient watchdog.
  kerryWdtLenient();
  bool ok = kerryCamGrabFreshJpegCopy(&jpeg, &len);
  if (ok) {
    ok = kerryLumaFromJpeg(jpeg, len, out);
    kerryCamFreeJpegCopy(jpeg);
  }
  kerryWdtRestore();
  return ok;
}
