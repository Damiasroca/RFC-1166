#pragma once

#include "kerry_cam_luma.h"
#include "kerry_shared.h"

#include <stdint.h>

typedef enum {
  KERRY_SCENE_OK = 0,
  KERRY_SCENE_CLIPPED_HIGH,
  KERRY_SCENE_CLIPPED_LOW,
  KERRY_SCENE_UNAVAILABLE
} kerry_scene_status_t;

typedef struct {
  kerry_scene_status_t status;
  float ev;
  float exposureMs;
  float gain;
} kerry_scene_t;

void kerryCamSceneCompute(const kerry_luma_t *luma, const kerry_cam_sensor_snapshot_t *snap,
                          const kerry_cam_exp_cfg_t *cfg, kerry_scene_t *out);

// Bits 7..4 each double the gain; bits 3..0 add n/16.
float kerryOv2640GainMultiplier(uint8_t gainReg);
