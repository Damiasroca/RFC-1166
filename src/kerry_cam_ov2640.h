#pragma once

#include <esp_camera.h>
#include <stdint.h>

bool kerryOv2640Is(uint16_t pid);

void kerryOv2640ForgetDefaults();
const char *kerryOv2640CaptureDefaults(sensor_t *s);
bool kerryOv2640DefaultsReady();
uint8_t kerryOv2640DefaultClkrc();

uint16_t kerryOv2640BaseFrameLines(framesize_t fs);
void kerryOv2640PeriodScale(uint8_t userClkrc, uint32_t *num, uint32_t *den);
uint32_t kerryOv2640BasePeriodMs(framesize_t fs);
void kerryOv2640NoteBaseline(framesize_t fs, uint8_t clkrc, uint16_t dummyLines, bool aecOn, uint16_t aecValue,
                             uint32_t measuredMs);

// Writes CLKRC and both dummy-line registers. Sentinel clock and zero dummy lines use the cached driver values.
const char *kerryOv2640ApplyTiming(sensor_t *s, uint8_t clkrc, uint16_t dummyLines);
int kerryOv2640SetAecValue(sensor_t *s, int value);
