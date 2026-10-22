#pragma once

#include "kerry_stats.h"

#include <ArduinoJson.h>
#include <stdint.h>

#define KERRY_PM_MAGIC 0x4B504D31u
#define KERRY_PM_VERSION 1
#define KERRY_PM_LINES 20
#define KERRY_PM_LINE_LEN 96

#define KERRY_ACT_UPLOAD (1u << 0)
#define KERRY_ACT_CAPTURE (1u << 1)
#define KERRY_ACT_STREAM (1u << 2)
#define KERRY_ACT_LIVEPUSH (1u << 3)
#define KERRY_ACT_OTA (1u << 4)
#define KERRY_ACT_SETTLE (1u << 5)
#define KERRY_ACT_ECOWITT (1u << 6)
#define KERRY_ACT_CAM_INIT (1u << 7)

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t lineHead;
  uint16_t lineCount;
  char lines[KERRY_PM_LINES][KERRY_PM_LINE_LEN];
  uint32_t uptimeS;
  uint32_t epoch;
  uint32_t heapFree;
  uint32_t heapMin;
  uint32_t heapLargest;
  uint32_t internalFree;
  uint32_t internalMin;
  uint32_t psramFree;
  uint32_t psramLargest;
  int8_t rssi;
  uint8_t wifiStatus;
  uint32_t activity;
  char cause[64];
  char fwGit[16];
  char fwVersion[24];
  kerry_stats_session_t stats;
} kerry_pm_rtc_t;

void kerryPmBoot();
void kerryPmReportsInit();
void kerryPmReportsOnClockSync();
void kerryPmLogLine(const char *line, int len);
void kerryPmSetActivity(uint32_t bit, bool on);
void kerryPmHeartbeat();
void kerryRebootWithCause(const char *cause);

bool kerryPmHadValidPrevious();
const kerry_pm_rtc_t *kerryPmPrevious();
void kerryPmFillReportsJson(JsonDocument &doc);
void kerryPmClearReportsAndCounters();
