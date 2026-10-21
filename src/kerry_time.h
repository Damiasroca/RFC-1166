#pragma once

#include <ArduinoJson.h>

void kerryTimeInit();
void kerryTimeLoop();
void kerryTimeOnWifiUp();
void kerryTimeUpdateFromCoords(double lat, double lon);
bool kerryTimeGetLocation(double *lat, double *lon, char *iana, size_t ianaLen);
void kerryTimeFillStatus(JsonDocument &doc);
