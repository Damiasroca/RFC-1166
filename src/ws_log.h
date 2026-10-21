#pragma once

#include <stddef.h>

#define KERRY_WS_LOG_PORT 82
#define KERRY_WS_LOG_CTRL_PORT 32770
#define KERRY_WS_LOG_PATH "/ws/log"

void kerry_ws_log_hook(void);
void kerry_ws_log_start(void);
void kerry_ws_log_write(const char *line, int len);
