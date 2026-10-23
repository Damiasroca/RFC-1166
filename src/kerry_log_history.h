#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t lastSeq;
  bool truncated;  // lines after sinceSeq were dropped, or sinceSeq predates a reboot
  bool more;       // out filled up; call again with sinceSeq = lastSeq
} kerry_log_read_t;

void kerryLogHistoryInit();
// tail: with sinceSeq 0, return the newest lines that fit instead of the oldest.
size_t kerryLogHistoryRead(uint32_t sinceSeq, bool tail, char *out, size_t cap, kerry_log_read_t *info);
void kerryLogHistoryWrite(const char *line, int len);
