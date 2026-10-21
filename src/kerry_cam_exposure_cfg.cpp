#include "kerry_cam_exposure_cfg.h"

#include "kerry_cam_ov2640.h"

#include <esp_camera.h>

kerry_cam_exp_cfg_t kerryCamExposureDefaultCfg() {
  kerry_cam_exp_cfg_t cfg = {};
  cfg.aecOn = true;
  cfg.aecValue = 600;
  cfg.aec2On = false;
  cfg.aeLevel = 0;
  cfg.agcOn = true;
  cfg.agcGain = 15;
  cfg.gainCeiling = KERRY_EXP_GAIN_CEIL_SENTINEL;
  cfg.clkrc = KERRY_EXP_CLKRC_SENTINEL;
  cfg.dummyLines = 0;
  return cfg;
}

bool kerryCamExposureCfgEqual(const kerry_cam_exp_cfg_t *a, const kerry_cam_exp_cfg_t *b) {
  return a->aecOn == b->aecOn && a->aecValue == b->aecValue && a->aec2On == b->aec2On && a->aeLevel == b->aeLevel &&
         a->agcOn == b->agcOn && a->agcGain == b->agcGain && a->gainCeiling == b->gainCeiling && a->clkrc == b->clkrc &&
         a->dummyLines == b->dummyLines;
}

int kerryCamExposureAeLevelMin(uint16_t pid) { return kerryOv2640Is(pid) ? -2 : -5; }

int kerryCamExposureAeLevelMax(uint16_t pid) { return kerryOv2640Is(pid) ? 2 : 5; }

uint16_t kerryCamExposureAecValueMax(uint16_t pid) { return kerryOv2640Is(pid) ? 1200 : 65535; }

bool kerryCamExposureClkrcAllowed(uint8_t v) {
  return v == KERRY_EXP_CLKRC_SENTINEL || v == 0x80 || v == 0xC0 || v == 0xE0;
}
