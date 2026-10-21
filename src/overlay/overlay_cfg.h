#pragma once

#include "overlay_nav.h"

extern bool g_ov_en;
extern ov_geom_t g_ov_geom;

void kerry_overlay_cfg_load();
void kerry_overlay_cfg_save(bool enabled, const ov_geom_t *geom);
int kerry_overlay_el_index(const char *id);
const char *kerry_overlay_el_id(int index);
