#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scanline JPEG encode through esp32-camera's jpge, quality 100, 4:2:0. */
esp_err_t ov_jpg_begin(int w, int h, uint8_t *dst, size_t cap);
esp_err_t ov_jpg_line(const uint8_t *rgb);
esp_err_t ov_jpg_end(size_t *out_n);
void ov_jpg_abort(void);

#ifdef __cplusplus
}
#endif
