#include "kerry_log.h"

#include "kerry_log_history.h"
#include "kerry_postmortem.h"
#include "ws_log.h"

#include <Arduino.h>

#include <stdio.h>
#include <string.h>

static KerryLogLevel s_level = KERRY_LOG_INFO;

struct rate_slot {
  const char *key;
  uint32_t last_ms;
};

struct change_slot {
  const char *key;
  char value[96];
};

static rate_slot s_rate[8];
static change_slot s_change[8];

static const char *level_name(KerryLogLevel level) {
  switch (level) {
    case KERRY_LOG_ERROR:
      return "error";
    case KERRY_LOG_WARN:
      return "warn";
    case KERRY_LOG_INFO:
      return "info";
    case KERRY_LOG_DEBUG:
      return "debug";
    default:
      return "?";
  }
}

void kerry_log_set_level(KerryLogLevel level) { s_level = level; }

void kerryLogSink(const char *line, int len) {
  kerryPmLogLine(line, len);
  kerryLogHistoryWrite(line, len);
}

void kerry_log_v(KerryLogLevel level, const char *tag, const char *fmt, va_list ap) {
  if (level > s_level || !fmt) return;
  char msg[220];
  vsnprintf(msg, sizeof msg, fmt, ap);
  char out[280];
  snprintf(out, sizeof out, "[%8lu] %s %-5s: %s\n", millis(), level_name(level), tag ? tag : "?", msg);
  Serial.print(out);
  int nout = (int)strlen(out);
  kerryLogSink(out, nout);
  kerry_ws_log_write(out, nout);
}

void kerry_log(KerryLogLevel level, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  kerry_log_v(level, tag, fmt, ap);
  va_end(ap);
}

bool kerry_log_every_ms(const char *key, uint32_t interval_ms) {
  if (!key || !interval_ms) return true;
  uint32_t now = millis();
  for (size_t i = 0; i < sizeof s_rate / sizeof s_rate[0]; i++) {
    if (s_rate[i].key == key || !s_rate[i].key) {
      if (!s_rate[i].key) s_rate[i].key = key;
      if ((uint32_t)(now - s_rate[i].last_ms) < interval_ms) return false;
      s_rate[i].last_ms = now;
      return true;
    }
  }
  return true;
}

bool kerry_log_if_changed(const char *key, const char *value) {
  if (!key) return true;
  const char *val = value ? value : "";
  for (size_t i = 0; i < sizeof s_change / sizeof s_change[0]; i++) {
    if (s_change[i].key == key || !s_change[i].key) {
      if (!s_change[i].key) s_change[i].key = key;
      if (strcmp(s_change[i].value, val) == 0) return false;
      strncpy(s_change[i].value, val, sizeof s_change[i].value - 1);
      s_change[i].value[sizeof s_change[i].value - 1] = '\0';
      return true;
    }
  }
  return true;
}

bool kerry_log_http_verbose(const char *uri) {
  if (!uri || !uri[0]) return true;
  if (strcmp(uri, "/capture") == 0) return false;
  if (strcmp(uri, "/stream") == 0) return false;
  if (strcmp(uri, "/app.css") == 0) return false;
  if (strcmp(uri, "/app.js") == 0) return false;
  if (strcmp(uri, "/api/auth") == 0) return false;
  if (strcmp(uri, "/api/network") == 0) return false;
  if (strcmp(uri, "/api/status") == 0) return false;
  if (strncmp(uri, "/api/log", 8) == 0) return false;
  if (strncmp(uri, "/favicon", 8) == 0) return false;
  return true;
}
