#include "kerry_cam_scene.h"

#include "kerry_cam_exposure.h"
#include "kerry_cam_exposure_cfg.h"
#include "kerry_cam_ov2640.h"

#include <math.h>
#include <string.h>

float kerryOv2640GainMultiplier(uint8_t gainReg) {
  unsigned coarse = 1;
  if (gainReg & 0x10) coarse *= 2;
  if (gainReg & 0x20) coarse *= 2;
  if (gainReg & 0x40) coarse *= 2;
  if (gainReg & 0x80) coarse *= 2;
  unsigned x16 = coarse * (16u + (unsigned)(gainReg & 0x0F));
  return (float)x16 / 16.0f;
}

static bool fillExposure(const kerry_cam_sensor_snapshot_t *snap, const kerry_cam_exp_cfg_t *cfg, kerry_scene_t *out) {
  uint32_t periodMs = kerryCamExposureMeasuredMs();
  if (periodMs == 0) periodMs = kerryCamExposureEstimatedMs();
  if (periodMs == 0) return false;
  uint32_t frameLines = (uint32_t)kerryOv2640BaseFrameLines(snap->framesize) + cfg->dummyLines;
  if (frameLines == 0) return false;
  float lineMs = (float)periodMs / (float)frameLines;
  float exposureMs = (float)snap->ov2640.aecValue * lineMs;
  if (exposureMs <= 0.0f) return false;
  out->exposureMs = exposureMs;
  out->gain = kerryOv2640GainMultiplier(snap->ov2640.gainReg);
  return true;
}

void kerryCamSceneCompute(const kerry_luma_t *luma, const kerry_cam_sensor_snapshot_t *snap,
                          const kerry_cam_exp_cfg_t *cfg, kerry_scene_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->status = KERRY_SCENE_UNAVAILABLE;
  if (!luma || !snap || !cfg) return;
  if (!kerryOv2640Is(snap->pid) || !snap->haveOv2640Live) return;

  kerry_scene_status_t clip = KERRY_SCENE_OK;
  if (luma->avg >= 245) clip = KERRY_SCENE_CLIPPED_HIGH;
  else if (luma->avg <= 3) clip = KERRY_SCENE_CLIPPED_LOW;

  if (!fillExposure(snap, cfg, out)) return;
  if (clip != KERRY_SCENE_OK) {
    out->status = clip;
    return;
  }

  float lin = (float)(luma->linMean > 0 ? luma->linMean : 1) / 65535.0f;
  out->ev = log2f(lin / ((out->exposureMs / 1000.0f) * out->gain));
  out->status = KERRY_SCENE_OK;
}
