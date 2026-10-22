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

size_t kerryLogHistoryRead(uint32_t sinceSeq, char *out, size_t cap, uint32_t *lastSeq, bool *truncated) {
  if (!out || cap < 2 || !lastSeq || !truncated) return 0;
  *lastSeq = sinceSeq;
  *truncated = false;
  if (!s_buf || !s_mu) return 0;
  if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
  *truncated = sinceSeq > 0 && sinceSeq < s_oldestSeq;
  size_t written = 0;
  size_t pos = s_head;
  size_t scanned = 0;
  while (scanned < s_used && written + 256 < cap) {
    size_t lineStart = pos;
    size_t lineLen = 0;
    while (scanned < s_used) {
      char c = s_buf[pos];
      pos = (pos + 1) % s_cap;
      scanned++;
      lineLen++;
      if (c == '\n') break;
    }
    if (lineLen < 2) continue;
    char seqBuf[16];
    size_t si = 0;
    for (size_t i = 0; i < lineLen - 1 && si + 1 < sizeof seqBuf; i++) {
      char c = s_buf[(lineStart + i) % s_cap];
      if (c == ':') break;
      seqBuf[si++] = c;
    }
    seqBuf[si] = '\0';
    uint32_t seq = (uint32_t)strtoul(seqBuf, nullptr, 10);
    if (seq <= sinceSeq) continue;
    *lastSeq = seq;
    for (size_t i = si + 1; i < lineLen - 1; i++) {
      if (written + 2 >= cap) break;
      out[written++] = s_buf[(lineStart + i) % s_cap];
    }
    if (written + 2 >= cap) break;
    out[written++] = '\n';
  }
  out[written] = '\0';
  xSemaphoreGive(s_mu);
  return written;
}
