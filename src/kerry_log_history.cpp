#include "kerry_log_history.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string.h>

#define HIST_INT 8192
#define HIST_PSRAM 65536

static char *s_buf = nullptr;
static size_t s_cap = 0;
static size_t s_head = 0;
static size_t s_used = 0;
static uint32_t s_seq = 0;
static uint32_t s_oldestSeq = 1;
static SemaphoreHandle_t s_mu;

void kerryLogHistoryInit() {
  if (s_mu) return;
  s_mu = xSemaphoreCreateMutex();
  s_cap = HIST_PSRAM;
  s_buf = (char *)heap_caps_malloc(s_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_buf) {
    s_cap = HIST_INT;
    s_buf = (char *)malloc(s_cap);
  }
  if (!s_buf) s_cap = 0;
}

static void dropOldestLine() {
  if (!s_used) return;
  size_t pos = s_head;
  size_t scanned = 0;
  while (scanned < s_used) {
    if (s_buf[pos] == '\n') {
      size_t lineLen = scanned + 1;
      s_head = (s_head + lineLen) % s_cap;
      s_used -= lineLen;
      s_oldestSeq++;
      return;
    }
    pos = (pos + 1) % s_cap;
    scanned++;
  }
  s_head = 0;
  s_used = 0;
  s_oldestSeq = s_seq + 1;
}

void kerryLogHistoryWrite(const char *line, int len) {
  if (!s_buf || !s_cap || !line || len <= 0) return;
  if (xSemaphoreTake(s_mu, 0) != pdTRUE) return;
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
  char prefix[16];
  int plen = snprintf(prefix, sizeof prefix, "%lu:", (unsigned long)(++s_seq));
  size_t need = (size_t)plen + (size_t)len + 2;
  while (s_used + need > s_cap) dropOldestLine();
  for (int i = 0; i < plen; i++) {
    size_t tail = (s_head + s_used) % s_cap;
    s_buf[tail] = prefix[i];
    s_used++;
  }
  for (int i = 0; i < len; i++) {
    size_t tail = (s_head + s_used) % s_cap;
    s_buf[tail] = line[i];
    s_used++;
  }
  size_t tail = (s_head + s_used) % s_cap;
  s_buf[tail] = '\n';
  s_used++;
  xSemaphoreGive(s_mu);
}

static inline char histAt(size_t rel) { return s_buf[(s_head + rel) % s_cap]; }

// First line start whose stored bytes (seq prefix included) fit in budget.
static size_t tailStart(size_t budget) {
  if (s_used <= budget) return 0;
  size_t rel = s_used - budget;
  while (rel < s_used && histAt(rel - 1) != '\n') rel++;
  return rel;
}

size_t kerryLogHistoryRead(uint32_t sinceSeq, bool tail, char *out, size_t cap, kerry_log_read_t *info) {
  if (!out || cap < 2 || !info) return 0;
  info->lastSeq = sinceSeq;
  info->truncated = false;
  info->more = false;
  out[0] = '\0';
  if (!s_buf || !s_mu) return 0;
  if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
  if (sinceSeq > s_seq) {
    // The client's cursor is from before a reboot.
    sinceSeq = 0;
    info->lastSeq = 0;
    info->truncated = true;
  }
  if (sinceSeq > 0 && sinceSeq + 1 < s_oldestSeq) info->truncated = true;
  size_t written = 0;
  size_t rel = (tail && sinceSeq == 0) ? tailStart(cap - 1) : 0;
  while (rel < s_used) {
    size_t lineStart = rel;
    while (rel < s_used && histAt(rel) != '\n') rel++;
    size_t lineEnd = rel;
    if (rel < s_used) rel++;
    uint32_t seq = 0;
    size_t i = lineStart;
    while (i < lineEnd && histAt(i) >= '0' && histAt(i) <= '9') {
      seq = seq * 10 + (uint32_t)(histAt(i) - '0');
      i++;
    }
    if (i < lineEnd && histAt(i) == ':') i++;
    if (seq <= sinceSeq) continue;
    size_t textLen = lineEnd - i;
    if (written + textLen + 1 >= cap) {
      if (written) {
        info->more = true;
        break;
      }
      textLen = cap - 2;
    }
    for (size_t k = 0; k < textLen; k++) out[written++] = histAt(i + k);
    out[written++] = '\n';
    info->lastSeq = seq;
  }
  out[written] = '\0';
  xSemaphoreGive(s_mu);
  return written;
}
