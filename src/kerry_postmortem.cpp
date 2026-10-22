#include "kerry_postmortem.h"

#include "kerry_stats.h"
#include "kerry_version.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <Arduino.h>
#include <WiFi.h>

#include <string.h>

static RTC_NOINIT_ATTR kerry_pm_rtc_t s_rtc;
static kerry_pm_rtc_t *s_prev = nullptr;
static bool s_hadPrev = false;
static portMUX_TYPE s_pmMux = portMUX_INITIALIZER_UNLOCKED;

static bool fieldTerminated(const char *s, size_t cap) {
  if (!s) return false;
  size_t n = strnlen(s, cap);
  return n < cap;
}

static bool pmRecordValid(const kerry_pm_rtc_t *pm) {
  if (!pm || pm->magic != KERRY_PM_MAGIC || pm->version != KERRY_PM_VERSION) return false;
  if (pm->lineHead >= KERRY_PM_LINES) return false;
  if (pm->lineCount > KERRY_PM_LINES) return false;
  if (!fieldTerminated(pm->cause, sizeof pm->cause)) return false;
  if (!fieldTerminated(pm->fwGit, sizeof pm->fwGit)) return false;
  if (!fieldTerminated(pm->fwVersion, sizeof pm->fwVersion)) return false;
  if (!fieldTerminated(pm->stats.lastUpFailText, sizeof pm->stats.lastUpFailText)) return false;
  for (uint16_t i = 0; i < pm->lineCount && i < KERRY_PM_LINES; i++) {
    if (!fieldTerminated(pm->lines[i], KERRY_PM_LINE_LEN)) return false;
  }
  return true;
}

static void pmInitSession() {
  memset(&s_rtc, 0, sizeof s_rtc);
  s_rtc.magic = KERRY_PM_MAGIC;
  s_rtc.version = KERRY_PM_VERSION;
  strncpy(s_rtc.fwGit, kerryFirmwareGit(), sizeof s_rtc.fwGit - 1);
  strncpy(s_rtc.fwVersion, kerryFirmwareVersion(), sizeof s_rtc.fwVersion - 1);
}

void kerryPmBoot() {
  kerry_pm_rtc_t scratch;
  memcpy(&scratch, &s_rtc, sizeof scratch);
  s_hadPrev = false;
  if (s_prev) {
    heap_caps_free(s_prev);
    s_prev = nullptr;
  }
  esp_reset_reason_t rr = esp_reset_reason();
  if (pmRecordValid(&scratch) && rr != ESP_RST_POWERON) {
    size_t sz = sizeof(kerry_pm_rtc_t);
    s_prev = (kerry_pm_rtc_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prev) s_prev = (kerry_pm_rtc_t *)malloc(sz);
    if (s_prev) {
      memcpy(s_prev, &scratch, sz);
      s_hadPrev = true;
    }
  }
  pmInitSession();
  kerryStatsBoot(s_hadPrev);
}

bool kerryPmHadValidPrevious() { return s_hadPrev; }

const kerry_pm_rtc_t *kerryPmPrevious() { return s_prev; }

kerry_stats_session_t *kerryStatsSessionPtr() { return &s_rtc.stats; }

void kerryPmLogLine(const char *line, int len) {
  if (!line || len <= 0) return;
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
  if (len <= 0) return;
  int copy = len;
  if (copy >= (int)KERRY_PM_LINE_LEN) copy = (int)KERRY_PM_LINE_LEN - 1;
  portENTER_CRITICAL(&s_pmMux);
  uint16_t idx = s_rtc.lineHead;
  memcpy(s_rtc.lines[idx], line, (size_t)copy);
  s_rtc.lines[idx][copy] = '\0';
  s_rtc.lineHead = (uint16_t)((idx + 1) % KERRY_PM_LINES);
  if (s_rtc.lineCount < KERRY_PM_LINES) s_rtc.lineCount++;
  portEXIT_CRITICAL(&s_pmMux);
}

void kerryPmSetActivity(uint32_t bit, bool on) {
  portENTER_CRITICAL(&s_pmMux);
  if (on) s_rtc.activity |= bit;
  else s_rtc.activity &= ~bit;
  portEXIT_CRITICAL(&s_pmMux);
}

void kerryPmHeartbeat() {
  s_rtc.uptimeS = (uint32_t)(millis() / 1000UL);
  time_t now = 0;
  time(&now);
  s_rtc.epoch = (now >= 1600000000) ? (uint32_t)now : 0;
  s_rtc.heapFree = esp_get_free_heap_size();
  s_rtc.heapMin = esp_get_minimum_free_heap_size();
  s_rtc.heapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  s_rtc.internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s_rtc.internalMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  s_rtc.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s_rtc.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (WiFi.status() == WL_CONNECTED) {
    s_rtc.rssi = (int8_t)WiFi.RSSI();
    s_rtc.wifiStatus = (uint8_t)WL_CONNECTED;
  } else {
    s_rtc.rssi = 0;
    s_rtc.wifiStatus = (uint8_t)WiFi.status();
  }
}

void kerryRebootWithCause(const char *cause) {
  if (cause) {
    portENTER_CRITICAL(&s_pmMux);
    strncpy(s_rtc.cause, cause, sizeof s_rtc.cause - 1);
    s_rtc.cause[sizeof s_rtc.cause - 1] = '\0';
    portEXIT_CRITICAL(&s_pmMux);
  }
  kerryPmHeartbeat();
  kerryStatsFlush();
  if (cause) Serial.printf("rebooting: %s\n", cause);
  else Serial.println("rebooting");
  Serial.flush();
  delay(400);
  esp_restart();
}
