#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

#define KERRY_EXP_GAIN_CEIL_SENTINEL 0xFF
#define KERRY_EXP_CLKRC_SENTINEL 0xFF

void kerryCamExposureLoad();
void kerryCamExposureSave();

void kerryCamExposureFillJson(JsonDocument &doc);
const char *kerryCamExposureUpdate(JsonObjectConst exp);

// Apply NVS settings to the sensor. Caller must hold camLock. cameraReady required.
const char *kerryCamExposureApply(bool discardFrames);

uint32_t kerryCamExposureEstimatedMs();
uint32_t kerryCamExposureFrameMs();
bool kerryCamExposureNeedsLenientWdt();

void kerryCamExposureNoteCaptureOk();
void kerryCamExposureNoteCaptureFail();

bool kerryCamExposureRevertPending();

// Caller must hold camLock.
void kerryCamExposureServiceRevert();
