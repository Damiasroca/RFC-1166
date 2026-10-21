#pragma once

#include <esp_camera.h>
#include <stdint.h>

bool kerryOv2640Is(uint16_t pid);

void kerryOv2640ForgetDefaults();
const char *kerryOv2640CaptureDefaults(sensor_t *s);
bool kerryOv2640DefaultsReady();

// Writes CLKRC and both dummy-line registers. Sentinel clock and zero dummy lines use the cached driver values.
const char *kerryOv2640ApplyTiming(sensor_t *s, uint8_t clkrc, uint16_t dummyLines);
