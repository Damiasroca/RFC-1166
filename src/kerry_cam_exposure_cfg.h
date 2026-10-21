#pragma once

#include <stdint.h>

#include "kerry_cam_exposure.h"

typedef struct {
  bool aecOn;
  uint16_t aecValue;
  bool aec2On;
  int8_t aeLevel;
  bool agcOn;
  uint8_t agcGain;
  uint8_t gainCeiling;
  uint8_t clkrc;
  uint16_t dummyLines;
} kerry_cam_exp_cfg_t;

kerry_cam_exp_cfg_t kerryCamExposureDefaultCfg();
bool kerryCamExposureCfgEqual(const kerry_cam_exp_cfg_t *a, const kerry_cam_exp_cfg_t *b);

int kerryCamExposureAeLevelMin(uint16_t pid);
int kerryCamExposureAeLevelMax(uint16_t pid);
uint16_t kerryCamExposureAecValueMax(uint16_t pid);
bool kerryCamExposureClkrcAllowed(uint8_t v);

uint32_t kerryCamExposureEstimateMs(const kerry_cam_exp_cfg_t *cfg);
const char *kerryCamExposureCfgValidate(const kerry_cam_exp_cfg_t *cfg, uint16_t pid);
