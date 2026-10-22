#pragma once

#include <stdint.h>

#include <esp_camera.h>

#define KERRY_EXP_GAIN_CEIL_SENTINEL 0xFF
#define KERRY_EXP_CLKRC_SENTINEL 0xFF

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
void kerryCamExposureDayCfg(const kerry_cam_exp_cfg_t *base, kerry_cam_exp_cfg_t *out);
bool kerryCamExposureCfgEqual(const kerry_cam_exp_cfg_t *a, const kerry_cam_exp_cfg_t *b);

int kerryCamExposureAeLevelMin(uint16_t pid);
int kerryCamExposureAeLevelMax(uint16_t pid);
uint16_t kerryCamExposureAecValueMax(const kerry_cam_exp_cfg_t *cfg, uint16_t pid, framesize_t fs);
bool kerryCamExposureClkrcAllowed(uint8_t v);

// Mirrors FB_GET_TIMEOUT in esp32-camera esp_camera.c.
#define KERRY_CAM_FB_GET_TIMEOUT_MS 4000

uint32_t kerryCamExposureBudgetMs();
uint32_t kerryCamExposureMeasuredMs();
void kerryCamExposureNoteMeasured(uint32_t periodMs, const kerry_cam_exp_cfg_t *cfg, uint16_t pid, framesize_t fs);
uint32_t kerryCamExposureEstimateMs(const kerry_cam_exp_cfg_t *cfg, uint16_t pid, framesize_t fs);
const char *kerryCamExposureCfgValidate(const kerry_cam_exp_cfg_t *cfg, uint16_t pid, framesize_t fs);
