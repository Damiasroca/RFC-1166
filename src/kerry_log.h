#pragma once

#include <stdarg.h>
#include <stdint.h>

enum KerryLogLevel {
  KERRY_LOG_ERROR = 0,
  KERRY_LOG_WARN = 1,
  KERRY_LOG_INFO = 2,
  KERRY_LOG_DEBUG = 3,
};

void kerry_log_set_level(KerryLogLevel level);
void kerry_log_v(KerryLogLevel level, const char *tag, const char *fmt, va_list ap);
void kerry_log(KerryLogLevel level, const char *tag, const char *fmt, ...);

#define KERRY_LOGE(tag, ...) kerry_log(KERRY_LOG_ERROR, tag, __VA_ARGS__)
#define KERRY_LOGW(tag, ...) kerry_log(KERRY_LOG_WARN, tag, __VA_ARGS__)
#define KERRY_LOGI(tag, ...) kerry_log(KERRY_LOG_INFO, tag, __VA_ARGS__)
#define KERRY_LOGD(tag, ...) kerry_log(KERRY_LOG_DEBUG, tag, __VA_ARGS__)

bool kerry_log_every_ms(const char *key, uint32_t interval_ms);
bool kerry_log_if_changed(const char *key, const char *value);
bool kerry_log_http_verbose(const char *uri);
void kerryLogSink(const char *line, int len);
