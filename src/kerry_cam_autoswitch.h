#pragma once

#include "kerry_cam_luma.h"
#include "kerry_cam_scene.h"

#include <ArduinoJson.h>
#include <stdint.h>

typedef struct {
  bool enabled;
  int16_t lowEvTenths;
  int16_t highEvTenths;
  uint8_t intervalMin;
  uint8_t samples;
  uint8_t minDwellMin;
} kerry_cam_autoswitch_cfg_t;

typedef enum {
  KERRY_CAM_MODE_UNKNOWN = 0,
  KERRY_CAM_MODE_DAY = 1,
  KERRY_CAM_MODE_NIGHT = 2
} kerry_cam_mode_t;

void kerryCamAutoSwitchLoad();
void kerryCamAutoSwitchService(uint32_t nowMs);
void kerryCamAutoSwitchNoteScene(uint32_t nowMs, const kerry_luma_t *luma, const kerry_scene_t *scene);
const char *kerryCamAutoSwitchSaveNight();
const char *kerryCamAutoSwitchApply(kerry_cam_mode_t mode, bool automatic, float triggerEv);
const char *kerryCamAutoSwitchSetCfg(const kerry_cam_autoswitch_cfg_t *cfg);
void kerryCamAutoSwitchFillJson(JsonDocument &doc);
