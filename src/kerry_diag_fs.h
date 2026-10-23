#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#define KERRY_DIAG_REPORT_MAX 2600
#define KERRY_DIAG_LOG_MAX 8192

void kerryDiagFsInit();
bool kerryDiagFsReady();
const char *kerryDiagStorageLabel();

uint32_t kerryDiagMaxReportSeq();
bool kerryDiagStoreReport(uint32_t bootSeq, const char *json, size_t len);
bool kerryDiagUpdateReport(uint32_t bootSeq, const char *json, size_t len);
// Newest first, header fields only (no log lines or core dump).
void kerryDiagListReports(JsonArray out);
// Full stored report JSON; returns 0 when there is no report for bootSeq.
size_t kerryDiagReadReport(uint32_t bootSeq, char *out, size_t cap);
void kerryDiagClearReports();

// Log tail written just before a deliberate reboot, attached to the next boot's report.
bool kerryDiagSaveLastLog(const char *text, size_t len);
void kerryDiagClaimLastLog(uint32_t bootSeq);
size_t kerryDiagReadLog(uint32_t bootSeq, char *out, size_t cap);

bool kerryDiagLoadFails(void *data, size_t len);
bool kerryDiagSaveFails(const void *data, size_t len);
