#pragma once

#include "kerry_cam_autoswitch.h"
#include "kerry_cam_luma.h"
#include "kerry_cam_scene.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t g_autoMu;
extern kerry_cam_autoswitch_cfg_t g_autoCfg;
extern kerry_cam_mode_t g_autoMode;
extern uint8_t g_autoLowCount;
extern uint8_t g_autoHighCount;
extern kerry_luma_t g_autoLastLuma;
extern bool g_autoHaveLast;
extern uint32_t g_autoLastSampleMs;
extern uint32_t g_autoNextSampleMs;
extern const char *g_autoLastSwitchReason;
extern uint32_t g_autoLastSwitchMs;
extern bool g_autoHaveLastSwitch;
extern kerry_scene_t g_autoLastScene;
extern bool g_autoHaveScene;

void kerryAutoEnsureMu();
const char *kerryAutoModeName(kerry_cam_mode_t mode);
kerry_cam_autoswitch_cfg_t kerryAutoDefaultCfg();
const char *kerryAutoValidateCfg(const kerry_cam_autoswitch_cfg_t *cfg);
void kerryAutoSaveCfg(const kerry_cam_autoswitch_cfg_t *cfg);
void kerryAutoSaveMode(kerry_cam_mode_t mode);
