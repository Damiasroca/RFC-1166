#pragma once

#include <ArduinoJson.h>

void kerryHealthInit();
void kerryHealthFillStatus(JsonDocument &doc);
const char *kerryHealthSetWatchdog(bool enabled, int camN, int upN, int capN, int capM);
void kerryHealthNoteCameraOk();
bool kerryHealthNoteCameraFail(const char *detail);
void kerryHealthNoteUploadOk();
bool kerryHealthNoteUploadFail(const char *detail);
bool kerryHealthNoteNetFail(const char *detail);
