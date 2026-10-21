#include "overlay.h"

#include "overlay_enc.h"
#include "overlay_layout.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <rom/tjpgd.h>

#include <stdlib.h>
#include <string.h>
extern bool g_ov_en;
extern ov_geom_t g_ov_geom;

typedef struct {
  const uint8_t *jpeg;
  size_t len;
  size_t pos;
  uint8_t *band;
  int w;
  int band_cap;
  int band_y;
  int rows;
  int y;
  int fail;
  ov_layout_t *layout;
} dec_t;

static StaticSemaphore_t s_mu_buf;
static SemaphoreHandle_t s_mu;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void *psram(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

static UINT in_cb(JDEC *jd, BYTE *buf, UINT n) {
  dec_t *d = jd->device;
  if (d->pos >= d->len || n == 0) return 0;
  if (n > d->len - d->pos) n = (UINT)(d->len - d->pos);
  if (buf) memcpy(buf, d->jpeg + d->pos, n);
  d->pos += n;
  return n;
}

static void flush_band(dec_t *d) {
  if (d->rows <= 0 || d->fail) return;
  ov_band_t band = {.rgb = d->band, .frame_w = d->w, .y0 = d->band_y, .rows = d->rows};
  ov_layout_draw(d->layout, &band);
  for (int i = 0; i < d->rows; i++) {
    if (ov_jpg_line(d->band + (size_t)i * (size_t)d->w * 3) != ESP_OK) {
      d->fail = 1;
      break;
    }
  }
  d->y = d->band_y + d->rows;
  d->rows = 0;
  vTaskDelay(1);
}

static UINT out_cb(JDEC *jd, void *bitmap, JRECT *rect) {
  dec_t *d = jd->device;
  int rw, rh;
  const uint8_t *src;
  if (d->fail) return 0;
  if (rect->left > rect->right || rect->top > rect->bottom) return 0;
  if (d->rows > 0 && rect->top != d->band_y) {
    flush_band(d);
    if (d->fail) return 0;
    if (d->y != rect->top) {
      d->fail = 1;
      return 0;
    }
  }
  if (d->rows == 0) {
    if (rect->top != d->y) {
      d->fail = 1;
      return 0;
    }
    d->band_y = rect->top;
  }
  rw = rect->right - rect->left + 1;
  rh = rect->bottom - rect->top + 1;
  if (rect->left + rw > d->w || rh > d->band_cap) {
    d->fail = 1;
    return 0;
  }
  src = bitmap;
  for (int row = 0; row < rh; row++) {
    int dy = rect->top - d->band_y + row;
    if (dy < 0 || dy >= d->band_cap) {
      d->fail = 1;
      return 0;
    }
    memcpy(d->band + ((size_t)dy * (size_t)d->w + (size_t)rect->left) * 3, src + (size_t)row * (size_t)rw * 3,
           (size_t)rw * 3);
  }
  if (rect->bottom - d->band_y + 1 > d->rows) d->rows = rect->bottom - d->band_y + 1;
  return 1;
}

bool kerry_overlay_enabled() { return g_ov_en; }

const ov_geom_t *kerry_overlay_geom() { return &g_ov_geom; }

static esp_err_t render_locked(const ov_nav_t *nav, const uint8_t *jpeg, size_t jpeg_len, uint8_t **out,
                               size_t *out_len) {
  dec_t d;
  JDEC jd;
  uint8_t *work = NULL;
  uint8_t *jpg = NULL;
  ov_nav_t local;
  size_t cap = 0;
  size_t jpg_n = 0;
  int w = 0, h = 0;
  esp_err_t err = ESP_FAIL;
  JRESULT jr;

  *out = NULL;
  *out_len = 0;
  if (!jpeg || jpeg_len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return ESP_FAIL;

  memset(&local, 0, sizeof local);
  if (nav) memcpy(&local, nav, sizeof local);

  memset(&d, 0, sizeof d);
  d.jpeg = jpeg;
  d.len = jpeg_len;
  work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!work) work = psram(4096);
  if (!work) return ESP_ERR_NO_MEM;

  jr = jd_prepare(&jd, in_cb, work, 4096, &d);
  if (jr != JDR_OK) {
    free(work);
    return ESP_FAIL;
  }
  w = (int)jd.width;
  h = (int)jd.height;
  if (w < 16 || h < 16 || w > 2048 || h > 1536) {
    free(work);
    return ESP_FAIL;
  }
  d.w = w;
  d.band_cap = (int)jd.msy * 8;
  if (d.band_cap < 8 || d.band_cap > 16) {
    free(work);
    return ESP_FAIL;
  }
  if (ov_layout_build(&d.layout, &g_ov_geom, w, h, &local) != 0) {
    free(work);
    return ESP_FAIL;
  }

  {
    size_t biggest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t pixels = (size_t)w * (size_t)h;
    cap = pixels;
    if (biggest > pixels + 65536) cap = biggest - 65536;
    if (cap > pixels * 2) cap = pixels * 2;
    if (cap < pixels / 2) {
      err = ESP_ERR_NO_MEM;
      goto done;
    }
  }
  jpg = psram(cap);
  d.band = psram((size_t)w * (size_t)d.band_cap * 3);
  if (!jpg || !d.band) {
    err = ESP_ERR_NO_MEM;
    goto done;
  }
  if (ov_jpg_begin(w, h, jpg, cap) != ESP_OK) goto done;

  jr = jd_decomp(&jd, out_cb, 0);
  if (jr == JDR_OK && !d.fail) flush_band(&d);
  if (jr != JDR_OK || d.fail || d.y != h) {
    ov_jpg_abort();
    goto done;
  }
  if (ov_jpg_end(&jpg_n) != ESP_OK || jpg_n < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) goto done;

  {
    uint8_t *tight = heap_caps_realloc(jpg, jpg_n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tight) jpg = tight;
  }
  *out = jpg;
  *out_len = jpg_n;
  jpg = NULL;
  err = ESP_OK;

done:
  ov_layout_free(d.layout);
  free(d.band);
  free(work);
  free(jpg);
  return err;
}

esp_err_t kerry_overlay_render(const ov_nav_t *nav, const uint8_t *jpeg, size_t jpeg_len, uint8_t **out,
                               size_t *out_len) {
  esp_err_t err;
  if (!out || !out_len) return ESP_ERR_INVALID_ARG;
  if (!g_ov_en) return ESP_ERR_INVALID_STATE;
  *out = NULL;
  *out_len = 0;
  if (!s_mu) {
    portENTER_CRITICAL(&s_init_mux);
    if (!s_mu) s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
    portEXIT_CRITICAL(&s_init_mux);
  }
  if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(120000)) != pdTRUE) return ESP_ERR_TIMEOUT;
  err = render_locked(nav, jpeg, jpeg_len, out, out_len);
  xSemaphoreGive(s_mu);
  return err;
}
