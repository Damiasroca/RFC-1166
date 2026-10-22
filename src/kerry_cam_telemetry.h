#pragma once

#include <stddef.h>
#include <stdint.h>

#define KERRY_CAM_TELEMETRY_INTERVAL_MS 60000UL

bool kerryCamTelemetryGrabCopy(uint8_t **jpeg, size_t *len);
void kerryCamTelemetryFreeCopy(uint8_t *jpeg);

void kerryCamTelemetryService(uint32_t now_ms);
