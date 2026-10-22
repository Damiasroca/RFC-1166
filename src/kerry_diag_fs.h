#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

void kerryDiagFsInit();
bool kerryDiagFsReady();
const char *kerryDiagStorageLabel();

void kerryDiagStoreReport(uint32_t bootSeq, const char *json, size_t len);
void kerryDiagUpdateReport(uint32_t bootSeq, const char *json, size_t len);
void kerryDiagListReports(JsonArray out);
void kerryDiagClearReports();

bool kerryDiagLoadFails(void *data, size_t len);
bool kerryDiagSaveFails(const void *data, size_t len);
