#pragma once

#include <esp_camera.h>
#include <stdint.h>

bool kerryOv2640Is(uint16_t pid);

// Writes CLKRC / dummy-line registers. Sentinel clock and zero dummy lines are left untouched.
int kerryOv2640ApplyTiming(sensor_t *s, uint8_t clkrc, uint16_t dummyLines);
