#pragma once

#include "overlay/overlay_nav.h"

#include <ArduinoJson.h>

void kerryGeoPulseLoadConfig();
void kerryGeoPulseSaveCredentials(const char *user, const char *pass, const char *device);
void kerryGeoPulseFillConfig(JsonDocument &doc);
const char *kerryGeoPulseApplyConfig(JsonDocument &in);

bool kerryGeoPulseFetchNav(ov_nav_t *nav);
const ov_nav_t *kerryGeoPulseLastNav();
bool kerryGeoPulseHasCredentials();
bool kerryGeoPulseEnsureNav(bool forceRefresh);
const ov_nav_t *kerryGeoPulseNavForOverlay();
uint32_t kerryGeoPulseCacheAgeMs();
const char *kerryGeoPulseLastError();
