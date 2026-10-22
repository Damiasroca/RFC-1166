#include "kerry_cam_autoswitch.h"

#include "kerry_cam_autoswitch_int.h"
#include "kerry_cam_exposure.h"
#include "kerry_cam_exposure_store.h"
#include "kerry_cam_luma.h"
#include "kerry_cam_ov2640.h"
#include "kerry_cam_scene.h"
#include "kerry_log.h"
#include "kerry_shared.h"

#include <Arduino.h>
#include <stdio.h>

static const char *TAG = "autosw";

SemaphoreHandle_t g_autoMu = nullptr;
kerry_cam_autoswitch_cfg_t g_autoCfg = {false, -60, -40, 2, 3, 15};
kerry_cam_mode_t g_autoMode = KERRY_CAM_MODE_UNKNOWN;
uint8_t g_autoLowCount = 0;
uint8_t g_autoHighCount = 0;
kerry_luma_t g_autoLastLuma = {};
bool g_autoHaveLast = false;
uint32_t g_autoLastSampleMs = 0;
uint32_t g_autoNextSampleMs = 0;
const char *g_autoLastSwitchReason = "";
uint32_t g_autoLastSwitchMs = 0;
bool g_autoHaveLastSwitch = false;
kerry_scene_t g_autoLastScene = {};
bool g_autoHaveScene = false;
static bool g_applying = false;

void kerryAutoEnsureMu() {
  if (!g_autoMu) g_autoMu = xSemaphoreCreateMutex();
}

const char *kerryAutoModeName(kerry_cam_mode_t mode) {
  if (mode == KERRY_CAM_MODE_DAY) return "day";
  if (mode == KERRY_CAM_MODE_NIGHT) return "night";
  return "unknown";
}

static void resetCounts() {
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  g_autoLowCount = 0;
  g_autoHighCount = 0;
  xSemaphoreGive(g_autoMu);
}

static bool beginApply() {
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  if (g_applying) {
    xSemaphoreGive(g_autoMu);
    return false;
  }
  g_applying = true;
  xSemaphoreGive(g_autoMu);
  return true;
}

static void endApply() {
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  g_applying = false;
  xSemaphoreGive(g_autoMu);
}

const char *kerryCamAutoSwitchSaveNight() {
  kerry_cam_exp_cfg_t cfg;
  kerryCamExpStoreGet(&cfg);
  kerryCamExpStoreSetNight(&cfg);
  return nullptr;
}

const char *kerryCamAutoSwitchApply(kerry_cam_mode_t mode) {
  if (mode != KERRY_CAM_MODE_DAY && mode != KERRY_CAM_MODE_NIGHT) return "Unknown mode";
  if (!beginApply()) return "Switch already in progress";
  kerry_cam_exp_cfg_t night;
  if (!kerryCamExpStoreGetNight(&night)) {
    xSemaphoreTake(g_autoMu, portMAX_DELAY);
    g_autoLastSwitchReason = "No night preset";
    g_autoLowCount = 0;
    g_autoHighCount = 0;
    xSemaphoreGive(g_autoMu);
    endApply();
    return "No night preset";
  }
  kerry_cam_exp_cfg_t cfg;
  if (mode == KERRY_CAM_MODE_NIGHT) cfg = night;
  else kerryCamExposureDayCfg(&night, &cfg);
  const char *err = kerryCamApplyExposureCfg(&cfg);
  if (err) {
    KERRY_LOGW(TAG, "switch to %s failed: %s", kerryAutoModeName(mode), err);
    resetCounts();
    xSemaphoreTake(g_autoMu, portMAX_DELAY);
    g_autoLastSwitchReason = err;
    xSemaphoreGive(g_autoMu);
    endApply();
    return err;
  }
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  g_autoMode = mode;
  g_autoLowCount = 0;
  g_autoHighCount = 0;
  g_autoLastSwitchReason = mode == KERRY_CAM_MODE_NIGHT ? "night" : "day";
  g_autoLastSwitchMs = millis();
  g_autoHaveLastSwitch = true;
  xSemaphoreGive(g_autoMu);
  kerryAutoSaveMode(mode);
  KERRY_LOGI(TAG, "switched to %s", kerryAutoModeName(mode));
  endApply();
  return nullptr;
}

const char *kerryCamAutoSwitchSetCfg(const kerry_cam_autoswitch_cfg_t *cfg) {
  const char *err = kerryAutoValidateCfg(cfg);
  if (err) return err;
  kerryAutoEnsureMu();
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  g_autoCfg = *cfg;
  g_autoLowCount = 0;
  g_autoHighCount = 0;
  g_autoNextSampleMs = millis();
  kerry_cam_autoswitch_cfg_t copy = g_autoCfg;
  xSemaphoreGive(g_autoMu);
  kerryAutoSaveCfg(&copy);
  return nullptr;
}

static const char *sceneStatusName(kerry_scene_status_t status) {
  switch (status) {
    case KERRY_SCENE_OK:
      return "ok";
    case KERRY_SCENE_CLIPPED_HIGH:
      return "clipped_high";
    case KERRY_SCENE_CLIPPED_LOW:
      return "clipped_low";
    default:
      return "unavailable";
  }
}

static void rememberSample(uint32_t nowMs, const kerry_luma_t *luma, const kerry_scene_t *scene) {
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  g_autoLastLuma = *luma;
  g_autoHaveLast = true;
  g_autoLastSampleMs = nowMs;
  g_autoLastScene = *scene;
  g_autoHaveScene = true;
  xSemaphoreGive(g_autoMu);
}

void kerryCamAutoSwitchService(uint32_t nowMs) {
  kerryAutoEnsureMu();
  kerry_cam_autoswitch_cfg_t cfg;
  uint32_t nextMs;
  bool haveLastSwitch;
  uint32_t lastSwitchMs;
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  cfg = g_autoCfg;
  nextMs = g_autoNextSampleMs;
  haveLastSwitch = g_autoHaveLastSwitch;
  lastSwitchMs = g_autoLastSwitchMs;
  xSemaphoreGive(g_autoMu);
  if (!cfg.enabled) return;
  kerry_cam_exp_cfg_t night;
  if (!kerryCamExpStoreGetNight(&night)) return;
  if ((int32_t)(nowMs - nextMs) < 0) return;
  if (!kerryCamSamplingAllowed()) return;

  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  g_autoNextSampleMs = nowMs + (uint32_t)cfg.intervalMin * 60000UL;
  xSemaphoreGive(g_autoMu);

  kerry_cam_sensor_snapshot_t snap;
  if (!kerryCamReadSensorSnapshot(&snap)) {
    if (kerry_log_every_ms("autosw_snap", 60000)) KERRY_LOGW(TAG, "scene EV unavailable");
    return;
  }
  if (!kerryOv2640Is(snap.pid)) {
    if (kerry_log_every_ms("autosw_ov2640", 60000)) KERRY_LOGW(TAG, "automatic switching needs an OV2640 sensor");
    return;
  }

  kerry_luma_t luma;
  if (!kerryCamSampleLuma(&luma)) {
    if (kerry_log_every_ms("autosw_sample", 60000)) KERRY_LOGW(TAG, "luma sample failed");
    return;
  }
  if (!kerryCamReadSensorSnapshot(&snap)) {
    if (kerry_log_every_ms("autosw_scene", 60000)) KERRY_LOGW(TAG, "scene EV unavailable");
    return;
  }
  kerry_cam_exp_cfg_t exp;
  kerryCamExpStoreGet(&exp);
  kerry_scene_t scene;
  kerryCamSceneCompute(&luma, &snap, &exp, &scene);
  if (scene.status == KERRY_SCENE_UNAVAILABLE) {
    rememberSample(nowMs, &luma, &scene);
    if (kerry_log_every_ms("autosw_scene", 60000)) KERRY_LOGW(TAG, "scene EV unavailable");
    return;
  }

  uint8_t lowCount = 0;
  uint8_t highCount = 0;
  bool toNight = false;
  bool toDay = false;
  kerry_cam_mode_t mode;
  xSemaphoreTake(g_autoMu, portMAX_DELAY);
  g_autoLastLuma = luma;
  g_autoHaveLast = true;
  g_autoLastSampleMs = nowMs;
  g_autoLastScene = scene;
  g_autoHaveScene = true;
  mode = g_autoMode;
  bool isLow = (scene.status == KERRY_SCENE_OK && scene.ev * 10.0f <= (float)cfg.lowEvTenths) ||
               (scene.status == KERRY_SCENE_CLIPPED_LOW && mode != KERRY_CAM_MODE_NIGHT);
  bool isHigh = (scene.status == KERRY_SCENE_OK && scene.ev * 10.0f >= (float)cfg.highEvTenths) ||
                (scene.status == KERRY_SCENE_CLIPPED_HIGH && mode != KERRY_CAM_MODE_DAY);
  if (mode != KERRY_CAM_MODE_NIGHT && isLow) {
    if (g_autoLowCount < 255) g_autoLowCount++;
  } else {
    g_autoLowCount = 0;
  }
  if (mode != KERRY_CAM_MODE_DAY && isHigh) {
    if (g_autoHighCount < 255) g_autoHighCount++;
  } else {
    g_autoHighCount = 0;
  }
  lowCount = g_autoLowCount;
  highCount = g_autoHighCount;
  toNight = g_autoLowCount >= cfg.samples;
  toDay = g_autoHighCount >= cfg.samples;
  xSemaphoreGive(g_autoMu);

  bool dwell = false;
  if ((toNight || toDay) && cfg.minDwellMin > 0 && haveLastSwitch) {
    uint32_t readyAt = lastSwitchMs + (uint32_t)cfg.minDwellMin * 60000UL;
    dwell = (int32_t)(nowMs - readyAt) < 0;
  }
  char evText[16];
  if (scene.status == KERRY_SCENE_OK) snprintf(evText, sizeof evText, "%.1f", scene.ev);
  else snprintf(evText, sizeof evText, "-");
  KERRY_LOGI(TAG, "luma avg=%u lin=%u scene=%s ev=%s mode=%s low=%u high=%u%s", (unsigned)luma.avg,
             (unsigned)luma.linMean, sceneStatusName(scene.status), evText, kerryAutoModeName(mode), (unsigned)lowCount,
             (unsigned)highCount, dwell ? " dwell" : "");
  if (dwell) return;
  if (toNight) kerryCamAutoSwitchApply(KERRY_CAM_MODE_NIGHT);
  else if (toDay) kerryCamAutoSwitchApply(KERRY_CAM_MODE_DAY);
}
