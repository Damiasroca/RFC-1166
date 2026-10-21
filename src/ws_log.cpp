/* WebSocket log stream on port 82 — isolated from the camera HTTP server (port 80). */

#include "ws_log.h"

#include "kerry_log.h"

#include <esp_http_server.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#if !CONFIG_HTTPD_WS_SUPPORT
#error "CONFIG_HTTPD_WS_SUPPORT must be enabled (see sdkconfig.defaults)"
#endif

static const char *TAG = "ws_log";

#define RING_SZ 4096
#define LOG_LINE_MAX 192
#define MAX_CLIENTS 3

static const char *SKIP_TAGS[] = {"ws_log", "websocket_client", "httpd_ws", "httpd"};

static char s_ring[RING_SZ];
static size_t s_rhead = 0;
static size_t s_rtail = 0;
static SemaphoreHandle_t s_ring_mu;
static TaskHandle_t s_task;
static volatile bool s_hooked = false;
static httpd_handle_t s_httpd = nullptr;
static volatile bool s_run = false;
static uint8_t s_level = ESP_LOG_INFO;

static int line_level(const char *s) {
  while (s && *s) {
    if (*s == '\033') {
      while (*s && *s != 'm') s++;
      if (*s == 'm') s++;
      continue;
    }
    if (*s == 'E' || *s == 'W' || *s == 'I' || *s == 'D' || *s == 'V') {
      switch (*s) {
        case 'E':
          return ESP_LOG_ERROR;
        case 'W':
          return ESP_LOG_WARN;
        case 'I':
          return ESP_LOG_INFO;
        case 'D':
          return ESP_LOG_DEBUG;
        case 'V':
          return ESP_LOG_VERBOSE;
      }
    }
    break;
  }
  return ESP_LOG_INFO;
}

static bool skip_send(const char *s) {
  for (size_t i = 0; i < sizeof SKIP_TAGS / sizeof SKIP_TAGS[0]; i++) {
    char needle[24];
    snprintf(needle, sizeof needle, " %s:", SKIP_TAGS[i]);
    if (strstr(s, needle)) return true;
  }
  return false;
}

static void ring_put(const char *s, int n) {
  if (!s_ring_mu || n <= 0) return;
  if (xSemaphoreTake(s_ring_mu, 0) != pdTRUE) return;
  for (int i = 0; i < n; i++) {
    size_t next = (s_rtail + 1) % RING_SZ;
    if (next == s_rhead) s_rhead = (s_rhead + 1) % RING_SZ;
    s_ring[s_rtail] = s[i];
    s_rtail = next;
  }
  xSemaphoreGive(s_ring_mu);
}

static int ring_pop(char *out, int cap) {
  if (!s_ring_mu || cap < 2) return 0;
  if (xSemaphoreTake(s_ring_mu, 0) != pdTRUE) return 0;
  int n = 0;
  while (s_rhead != s_rtail && n < cap - 1) {
    char c = s_ring[s_rhead];
    s_rhead = (s_rhead + 1) % RING_SZ;
    out[n++] = c;
    if (c == '\n') break;
  }
  xSemaphoreGive(s_ring_mu);
  out[n] = 0;
  return n;
}

static int ws_vprintf(const char *fmt, va_list ap) {
  if (s_run) {
    char line[LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > 0) {
      if (n >= (int)sizeof line) {
        line[sizeof line - 2] = '\n';
        n = (int)sizeof line - 1;
        line[n] = 0;
      }
      if ((uint8_t)line_level(line) <= s_level && !skip_send(line)) {
        ring_put(line, n);
        if (s_task) xTaskNotifyGive(s_task);
      }
    }
  }
  return vprintf(fmt, ap);
}

void kerry_ws_log_write(const char *line, int len) {
  if (!s_run || !line || len <= 0) return;
  ring_put(line, len);
  if (s_task) xTaskNotifyGive(s_task);
}

static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) return ESP_OK;
  httpd_ws_frame_t f = {};
  if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
  if (f.len == 0) return ESP_OK;
  uint8_t *buf = (uint8_t *)calloc(1, f.len + 1);
  if (!buf) return ESP_ERR_NO_MEM;
  f.payload = buf;
  esp_err_t e = httpd_ws_recv_frame(req, &f, f.len);
  free(buf);
  return e;
}

static void listen_stop() {
  if (!s_httpd) return;
  httpd_stop(s_httpd);
  s_httpd = nullptr;
}

static void listen_start() {
  listen_stop();
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = KERRY_WS_LOG_PORT;
  cfg.ctrl_port = KERRY_WS_LOG_CTRL_PORT;
  cfg.max_open_sockets = MAX_CLIENTS;
  cfg.max_uri_handlers = 2;
  cfg.lru_purge_enable = true;
  cfg.stack_size = 4096;
  cfg.recv_wait_timeout = 10;
  cfg.send_wait_timeout = 10;
  if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
    s_httpd = nullptr;
    ESP_LOGW(TAG, "listen httpd_start failed");
    return;
  }
  httpd_uri_t u = {
      .uri = KERRY_WS_LOG_PATH,
      .method = HTTP_GET,
      .handler = ws_handler,
      .is_websocket = true,
  };
  if (httpd_register_uri_handler(s_httpd, &u) != ESP_OK) {
    ESP_LOGW(TAG, "register %s failed", KERRY_WS_LOG_PATH);
    listen_stop();
  }
}

static void broadcast(const char *line, int n) {
  if (!s_httpd || n <= 0) return;
  httpd_ws_frame_t pkt = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *)line,
      .len = (size_t)n,
  };
  size_t nfds = MAX_CLIENTS;
  int fds[MAX_CLIENTS];
  if (httpd_get_client_list(s_httpd, &nfds, fds) != ESP_OK) return;
  for (size_t i = 0; i < nfds; i++) {
    if (httpd_ws_get_fd_info(s_httpd, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
    httpd_ws_send_data(s_httpd, fds[i], &pkt);
  }
}

static void try_listen() {
  if (s_httpd) return;
  listen_start();
  if (!s_httpd) return;
  if (!s_run) {
    s_run = true;
    KERRY_LOGI(TAG, "listening on ws://<camera-ip>:%u%s", KERRY_WS_LOG_PORT, KERRY_WS_LOG_PATH);
  }
}

static void log_task(void *arg) {
  (void)arg;
  try_listen();
  TickType_t heal_at = xTaskGetTickCount();
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    if (!s_httpd && (xTaskGetTickCount() - heal_at) > pdMS_TO_TICKS(5000)) {
      try_listen();
      heal_at = xTaskGetTickCount();
    }
    char line[LOG_LINE_MAX];
    int n;
    int burst = 0;
    while (burst++ < 32 && (n = ring_pop(line, sizeof line)) > 0) {
      broadcast(line, n);
    }
  }
}

void kerry_ws_log_hook() {
  if (!s_ring_mu) s_ring_mu = xSemaphoreCreateMutex();
  if (!s_hooked) {
    esp_log_set_vprintf(ws_vprintf);
    s_hooked = true;
  }
}

void kerry_ws_log_start() {
  kerry_ws_log_hook();
  if (s_task) return;
  xTaskCreate(log_task, "ws_log", 4096, nullptr, 2, &s_task);
  if (s_task) xTaskNotifyGive(s_task);
}
