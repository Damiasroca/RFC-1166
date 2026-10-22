#pragma once

#include <stddef.h>
#include <stdint.h>

#define KERRY_CAM_TELEMETRY_INTERVAL_MS 60000UL

void kerryCamTelemetryService(uint32_t now_ms);
