#pragma once

#include "esp_err.h"
#include "overlay_nav.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool kerry_overlay_enabled();
const ov_geom_t *kerry_overlay_geom();

esp_err_t kerry_overlay_render(const ov_nav_t *nav, const uint8_t *jpeg, size_t jpeg_len, uint8_t **out,
                               size_t *out_len);

#ifdef __cplusplus
}
#endif
