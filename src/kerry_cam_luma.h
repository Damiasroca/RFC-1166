#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t avg;
  uint8_t min;
  uint8_t max;
} kerry_luma_t;

bool kerryLumaFromJpeg(const uint8_t *jpeg, size_t len, kerry_luma_t *out);
bool kerryCamSampleLuma(kerry_luma_t *out);
