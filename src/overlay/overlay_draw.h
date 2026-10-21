#pragma once

#include "overlay_font.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t *rgb;
  int frame_w;
  int y0;
  int rows;
} ov_band_t;

void ov_fill_rect(ov_band_t *b, int x, int y, int w, int h, int r, int g, int bl, int a);
void ov_draw_text(ov_band_t *b, const ov_font_t *font, int x, int y, const char *utf8);
int ov_text_width(const ov_font_t *font, const char *utf8);
const ov_font_t *ov_font_pick(int frame_h);

#ifdef __cplusplus
}
#endif
