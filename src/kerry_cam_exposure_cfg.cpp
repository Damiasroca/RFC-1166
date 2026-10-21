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
  if (v == KERRY_EXP_CLKRC_SENTINEL) return true;
  if ((v & 0xC0) != 0x80) return false;
  uint8_t div = v & 0x3F;
  return div == 1 || div == 3 || div == 7 || div == 15;
}

uint32_t kerryCamExposureEstimateMs(const kerry_cam_exp_cfg_t *cfg) {
  uint32_t ms = 120;
  if (!cfg->aecOn) {
    ms = 80 + ((uint32_t)cfg->aecValue * 2500UL) / 1200UL;
  } else if (cfg->aec2On) {
    ms = 350;
  }
  if (cfg->clkrc == 0xC0) ms = (ms * 3) / 2;
  else if (cfg->clkrc == 0xE0) ms *= 2;
  if (cfg->dummyLines > 0) ms += (uint32_t)cfg->dummyLines / 2;
  return ms;
}

const char *kerryCamExposureCfgValidate(const kerry_cam_exp_cfg_t *cfg, uint16_t pid) {
  if (cfg->aecValue > kerryCamExposureAecValueMax(pid)) return "aecValue out of range";
  if (cfg->aeLevel < kerryCamExposureAeLevelMin(pid) || cfg->aeLevel > kerryCamExposureAeLevelMax(pid))
    return "aeLevel out of range";
  if (cfg->agcGain > 30) return "agcGain out of range";
  if (cfg->gainCeiling != KERRY_EXP_GAIN_CEIL_SENTINEL && cfg->gainCeiling > GAINCEILING_128X)
    return "gainCeiling out of range";
  if (!kerryCamExposureClkrcAllowed(cfg->clkrc)) return "clkrc not allowed";
  if (cfg->dummyLines > 512) return "dummyLines out of range";
  if (kerryCamExposureEstimateMs(cfg) > 10000)
    return "Exposure timing too long; reduce manual exposure or dummy lines";
  return nullptr;
}
