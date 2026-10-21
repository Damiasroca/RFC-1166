#include "overlay_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int on;
  int x, y, w, h;
  int text_x, text_y;
  char text[40];
  const ov_font_t *font;
} ov_label_t;

struct ov_layout {
  ov_label_t labels[OV_EL_N];
};

static void anchor_xy(int an, int fw, int fh, int ox, int oy, int *x, int *y) {
  int ax = 0;
  int ay = 0;
  if (an == OV_AN_TC || an == OV_AN_BC) ax = fw / 2;
  else if (an == OV_AN_TR || an == OV_AN_CR || an == OV_AN_BR) ax = fw;
  if (an == OV_AN_CL || an == OV_AN_CR) ay = fh / 2;
  else if (an == OV_AN_BL || an == OV_AN_BC || an == OV_AN_BR) ay = fh;
  *x = ax + ox * fw / 1000;
  *y = ay + oy * fh / 1000;
}

static void pin_box(int an, int ax, int ay, int bw, int bh, int *x, int *y) {
  if (an == OV_AN_TR || an == OV_AN_CR || an == OV_AN_BR) ax -= bw;
  else if (an == OV_AN_TC || an == OV_AN_BC) ax -= bw / 2;
  if (an == OV_AN_BL || an == OV_AN_BC || an == OV_AN_BR) ay -= bh;
  else if (an == OV_AN_CL || an == OV_AN_CR) ay -= bh / 2;
  *x = ax;
  *y = ay;
}

static void place_label(ov_label_t *lab, int an, int ox, int oy, int fw, int fh, const ov_font_t *font,
                        const char *text) {
  int ax, ay, pad_x, pad_y, tw;
  memset(lab, 0, sizeof *lab);
  if (!text || !text[0] || !font) return;
  pad_x = font->px / 3;
  pad_y = font->px / 6;
  if (pad_x < 2) pad_x = 2;
  if (pad_y < 1) pad_y = 1;
  tw = ov_text_width(font, text);
  lab->w = tw + pad_x * 2;
  lab->h = pad_y + font->ascent + font->descent + pad_y;
  anchor_xy(an, fw, fh, ox, oy, &ax, &ay);
  pin_box(an, ax, ay, lab->w, lab->h, &lab->x, &lab->y);
  lab->text_x = lab->x + pad_x;
  lab->text_y = lab->y + pad_y;
  lab->font = font;
  lab->on = 1;
  snprintf(lab->text, sizeof lab->text, "%s", text);
}

void ov_geom_defaults(ov_geom_t *g) {
  memset(g, 0, sizeof *g);
  for (int i = 0; i < OV_EL_N; i++) g->en[i] = 1;
  g->an[OV_EL_TIME] = OV_AN_TL;
  g->ox[OV_EL_TIME] = 12;
  g->oy[OV_EL_TIME] = 10;
  g->an[OV_EL_LAT] = OV_AN_TR;
  g->ox[OV_EL_LAT] = -12;
  g->oy[OV_EL_LAT] = 10;
  g->an[OV_EL_LON] = OV_AN_TR;
  g->ox[OV_EL_LON] = -12;
  g->oy[OV_EL_LON] = 56;
  g->an[OV_EL_SPD] = OV_AN_TR;
  g->ox[OV_EL_SPD] = -12;
  g->oy[OV_EL_SPD] = 102;
  g->an[OV_EL_CRS] = OV_AN_TR;
  g->ox[OV_EL_CRS] = -12;
  g->oy[OV_EL_CRS] = 148;
  g->an[OV_EL_DEV] = OV_AN_BL;
  g->ox[OV_EL_DEV] = 12;
  g->oy[OV_EL_DEV] = -12;
  g->an[OV_EL_MTC] = OV_AN_BL;
  g->ox[OV_EL_MTC] = 12;
  g->oy[OV_EL_MTC] = -56;
  g->an[OV_EL_REQ] = OV_AN_BL;
  g->ox[OV_EL_REQ] = 12;
  g->oy[OV_EL_REQ] = -100;
}

static const char *nav_text(const ov_nav_t *nav, int el) {
  if (!nav) return "";
  switch (el) {
    case OV_EL_TIME:
      return nav->time_text;
    case OV_EL_LAT:
      return nav->lat;
    case OV_EL_LON:
      return nav->lon;
    case OV_EL_SPD:
      return nav->speed;
    case OV_EL_CRS:
      return nav->course;
    case OV_EL_DEV:
      return nav->device_id;
    case OV_EL_MTC:
      return nav->match_type;
    case OV_EL_REQ:
      return nav->requested;
    default:
      return "";
  }
}

int ov_layout_build(ov_layout_t **out, const ov_geom_t *g, int fw, int fh, const ov_nav_t *nav) {
  ov_layout_t *L;
  const ov_font_t *font;
  *out = NULL;
  if (!g || fw < 16 || fh < 16) return -1;
  L = calloc(1, sizeof *L);
  if (!L) return -1;
  font = ov_font_pick(fh);
  for (int i = 0; i < OV_EL_N; i++) {
    const char *text;
    if (!g->en[i]) continue;
    text = nav_text(nav, i);
    if (!text[0]) continue;
    place_label(&L->labels[i], g->an[i], g->ox[i], g->oy[i], fw, fh, font, text);
  }
  *out = L;
  return 0;
}

void ov_layout_draw(const ov_layout_t *L, ov_band_t *band) {
  if (!L || !band) return;
  for (int i = 0; i < OV_EL_N; i++) {
    const ov_label_t *lab = &L->labels[i];
    if (!lab->on) continue;
    ov_fill_rect(band, lab->x, lab->y, lab->w, lab->h, 0, 0, 0, 160);
    ov_draw_text(band, lab->font, lab->text_x, lab->text_y, lab->text);
  }
}

void ov_layout_free(ov_layout_t *L) { free(L); }
