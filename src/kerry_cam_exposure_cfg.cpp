#include "kerry_cam_exposure_cfg.h"

#include "kerry_cam_ov2640.h"

#include <esp_camera.h>
#include <stdio.h>

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

uint16_t kerryCamExposureAecValueMax(const kerry_cam_exp_cfg_t *cfg, uint16_t pid, framesize_t fs) {
  if (!kerryOv2640Is(pid)) return 65535;
  uint32_t lines = (uint32_t)kerryOv2640BaseFrameLines(fs) + (cfg ? cfg->dummyLines : 0);
  if (lines > 65535) lines = 65535;
  if (lines == 0) return 1;
  return (uint16_t)lines;
}

bool kerryCamExposureClkrcAllowed(uint8_t v) {
  if (v == KERRY_EXP_CLKRC_SENTINEL) return true;
  if ((v & 0xC0) != 0x80) return false;
  uint8_t div = v & 0x3F;
  return div == 1 || div == 3 || div == 7 || div == 15;
}

uint32_t kerryCamExposureBudgetMs() { return (4000UL * 3UL) / 4UL; }

static uint32_t g_measuredMs = 0;
static uint32_t g_simpleAtMeasure = 0;

static uint32_t simpleEstimateMs(const kerry_cam_exp_cfg_t *cfg) {
  if (!cfg->aecOn) return 80 + ((uint32_t)cfg->aecValue * 2500UL) / 1200UL;
  if (cfg->aec2On) return 350;
  return 120;
}

uint32_t kerryCamExposureMeasuredMs() { return g_measuredMs; }

void kerryCamExposureNoteMeasured(uint32_t periodMs, const kerry_cam_exp_cfg_t *cfg, uint16_t pid, framesize_t fs) {
  g_measuredMs = periodMs;
  if (periodMs == 0 || !cfg) return;
  if (kerryOv2640Is(pid)) {
    kerryOv2640NoteBaseline(fs, cfg->clkrc, cfg->dummyLines, cfg->aecOn, cfg->aecValue, periodMs);
  } else {
    g_simpleAtMeasure = simpleEstimateMs(cfg);
  }
}

static uint32_t ov2640EstimateMs(const kerry_cam_exp_cfg_t *cfg, framesize_t fs) {
  uint32_t base = kerryOv2640BaseFrameLines(fs);
  uint32_t lines = base + cfg->dummyLines;
  if (!cfg->aecOn && cfg->aecValue > lines) lines = cfg->aecValue;
  uint32_t num = 1;
  uint32_t den = 1;
  kerryOv2640PeriodScale(cfg->clkrc, &num, &den);
  uint32_t basePeriod = kerryOv2640BasePeriodMs(fs);
  if (basePeriod == 0) {
    uint32_t tline = 1922;
    if ((int)fs <= (int)FRAMESIZE_CIF) tline = 595;
    else if ((int)fs <= (int)FRAMESIZE_SVGA) tline = 1190;
    uint32_t xclk = 16000000;
    sensor_t *s = esp_camera_sensor_get();
    if (s && s->xclk_freq_hz > 1000000) xclk = (uint32_t)s->xclk_freq_hz;
    basePeriod = (uint32_t)(((uint64_t)base * tline * 1000ULL) / xclk);
    if (basePeriod == 0) basePeriod = 1;
  }
  if (base == 0 || den == 0) return basePeriod;
  uint64_t ms = (uint64_t)basePeriod * num * lines / ((uint64_t)den * base);
  if (ms == 0) return 1;
  if (ms > 0xFFFFFFFFULL) return 0xFFFFFFFFUL;
  return (uint32_t)ms;
}

uint32_t kerryCamExposureEstimateMs(const kerry_cam_exp_cfg_t *cfg, uint16_t pid, framesize_t fs) {
  if (kerryOv2640Is(pid)) return ov2640EstimateMs(cfg, fs);
  uint32_t simple = simpleEstimateMs(cfg);
  if (g_measuredMs > 0 && g_simpleAtMeasure > 0)
    return (uint32_t)((uint64_t)g_measuredMs * simple / g_simpleAtMeasure);
  return simple;
}

const char *kerryCamExposureCfgValidate(const kerry_cam_exp_cfg_t *cfg, uint16_t pid, framesize_t fs) {
  if (cfg->aecValue > kerryCamExposureAecValueMax(cfg, pid, fs)) return "aecValue out of range";
  if (cfg->aeLevel < kerryCamExposureAeLevelMin(pid) || cfg->aeLevel > kerryCamExposureAeLevelMax(pid))
    return "aeLevel out of range";
  if (cfg->agcGain > 30) return "agcGain out of range";
  if (cfg->gainCeiling != KERRY_EXP_GAIN_CEIL_SENTINEL && cfg->gainCeiling > GAINCEILING_128X)
    return "gainCeiling out of range";
  if (!kerryCamExposureClkrcAllowed(cfg->clkrc)) return "clkrc not allowed";
  if (cfg->dummyLines > 512) return "dummyLines out of range";
  uint32_t estimate = kerryCamExposureEstimateMs(cfg, pid, fs);
  uint32_t budget = kerryCamExposureBudgetMs();
  if (estimate > budget) {
    static char msg[96];
    snprintf(msg, sizeof msg, "Exposure timing too long (%u ms estimate, limit %u ms)", (unsigned)estimate,
             (unsigned)budget);
    return msg;
  }
  return nullptr;
}
