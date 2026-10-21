#pragma once

#include <ArduinoJson.h>
#include <esp_camera.h>
#include <stdint.h>

#include "kerry_cam_exposure_cfg.h"
#include "kerry_cam_exposure_store.h"

void kerryCamExposureFillJson(JsonDocument &doc);
const char *kerryCamExposureStage(JsonObjectConst exp, framesize_t targetFs);
// Apply NVS settings to the sensor. Caller must hold camLock. cameraReady required.
const char *kerryCamExposureApply(bool discardFrames);

uint32_t kerryCamExposureEstimatedMs();
uint32_t kerryCamExposureFrameMs();

// Caller must hold camLock.
void kerryCamExposureServiceRevert();
