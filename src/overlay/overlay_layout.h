#pragma once

#include "overlay_draw.h"
#include "overlay_nav.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ov_layout ov_layout_t;

void ov_geom_defaults(ov_geom_t *g);
int ov_layout_build(ov_layout_t **out, const ov_geom_t *g, int fw, int fh, const ov_nav_t *nav);
void ov_layout_draw(const ov_layout_t *L, ov_band_t *band);
void ov_layout_free(ov_layout_t *L);

#ifdef __cplusplus
}
#endif
