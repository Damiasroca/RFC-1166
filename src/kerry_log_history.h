#pragma once

#include <stddef.h>
#include <stdint.h>

void kerryLogHistoryInit();
size_t kerryLogHistoryRead(uint32_t sinceSeq, char *out, size_t cap, uint32_t *lastSeq, bool *truncated);
void kerryLogHistoryWrite(const char *line, int len);
