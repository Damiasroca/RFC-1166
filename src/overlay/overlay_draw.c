#include "overlay_draw.h"

#include <string.h>

static void blend(uint8_t *p, int r, int g, int b, int a) {
  if (a <= 0) return;
  if (a >= 255) {
    p[0] = (uint8_t)r;
    p[1] = (uint8_t)g;
    p[2] = (uint8_t)b;
    return;
  }
  p[0] = (uint8_t)((r * a + p[0] * (255 - a) + 127) / 255);
  p[1] = (uint8_t)((g * a + p[1] * (255 - a) + 127) / 255);
  p[2] = (uint8_t)((b * a + p[2] * (255 - a) + 127) / 255);
}

static uint8_t *at(ov_band_t *b, int x, int y) {
  if (!b->rgb || x < 0 || y < b->y0 || y >= b->y0 + b->rows) return NULL;
  if ((unsigned)x >= (unsigned)b->frame_w) return NULL;
  return b->rgb + ((size_t)(y - b->y0) * (size_t)b->frame_w + (size_t)x) * 3;
}

void ov_fill_rect(ov_band_t *b, int x, int y, int w, int h, int r, int g, int bl, int a) {
  int x1, y1;
  if (w <= 0 || h <= 0 || a <= 0) return;
  x1 = x + w;
  y1 = y + h;
  if (x < 0) x = 0;
  if (y < b->y0) y = b->y0;
  if (x1 > b->frame_w) x1 = b->frame_w;
  if (y1 > b->y0 + b->rows) y1 = b->y0 + b->rows;
  for (int yy = y; yy < y1; yy++) {
    uint8_t *row = at(b, x, yy);
    if (!row) continue;
    for (int xx = x; xx < x1; xx++, row += 3) blend(row, r, g, bl, a);
  }
}

static const ov_glyph_t *glyph(const ov_font_t *font, unsigned cp) {
  for (uint16_t i = 0; i < font->n; i++) {
    if (font->glyphs[i].cp == cp) return &font->glyphs[i];
  }
  return NULL;
}

static unsigned next_cp(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  if (!s || !*s) return 0;
  if (s[0] < 0x80) {
    *p += 1;
    return s[0];
  }
  if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    *p += 2;
    return ((unsigned)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
  }
  *p += 1;
  return 0;
}

int ov_text_width(const ov_font_t *font, const char *utf8) {
  int w = 0;
  const char *p = utf8 ? utf8 : "";
  while (*p) {
    unsigned cp = next_cp(&p);
    const ov_glyph_t *g;
    if (!cp) continue;
    g = glyph(font, cp);
    w += g ? g->adv : font->px / 2;
  }
  return w;
}

const ov_font_t *ov_font_pick(int frame_h) {
  int target = 30 * frame_h / 1200;
  const ov_font_t *best = &ov_fonts[0];
  int best_d = 100000;
  if (target < 1) target = 1;
  for (int i = 0; i < ov_font_n; i++) {
    int d = (int)ov_fonts[i].px - target;
    if (d < 0) d = -d;
    if (d < best_d) {
      best_d = d;
      best = &ov_fonts[i];
    }
  }
  return best;
}

void ov_draw_text(ov_band_t *b, const ov_font_t *font, int x, int y, const char *utf8) {
  const char *p = utf8 ? utf8 : "";
  int pen = x;
  while (*p) {
    unsigned cp = next_cp(&p);
    const ov_glyph_t *g;
    const uint8_t *bits;
    if (!cp) continue;
    g = glyph(font, cp);
    if (!g) {
      pen += font->px / 2;
      continue;
    }
    bits = font->bits + g->off;
    for (int row = 0; row < g->h; row++) {
      int yy = y + g->yoff + row;
      for (int col = 0; col < g->w; col++) {
        int i = row * ((g->w + 1) / 2) + col / 2;
        int nib = (col & 1) ? (bits[i] & 15) : (bits[i] >> 4);
        uint8_t *px;
        if (!nib) continue;
        px = at(b, pen + g->xoff + col, yy);
        if (px) blend(px, 255, 255, 255, nib * 17);
      }
    }
    pen += g->adv;
  }
}
