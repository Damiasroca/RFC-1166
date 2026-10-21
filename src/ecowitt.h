#pragma once

#include <ArduinoJson.h>

void ecowittInit();
void ecowittFillStatus(JsonDocument &doc);
const char *ecowittSummary();

const char *ecowittSetSchedule(bool enabled, int intervalCode);
const char *ecowittQueueLogin(const char *account, const char *password);
const char *ecowittQueueRegister(const char *name);
const char *ecowittQueueLogout();
const char *ecowittQueueUpload();
