#include "web_diag_api.h"

#include "ecowitt.h"
#include "kerry_postmortem.h"
#include "kerry_shared.h"
#include "kerry_stats.h"
#include "kerry_log_history.h"
#include "kerry_web_util.h"

#include <esp_heap_caps.h>

#include <ArduinoJson.h>

#include <stdlib.h>
#include <string.h>

static esp_err_t apiPostmortemGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  kerryPmFillReportsJson(doc);
  return sendJson(req, doc);
}

static esp_err_t apiPostmortemPost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  JsonDocument out;
  if (deserializeJson(in, readBody(req, 256))) {
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }
  if (strcmp(in["action"] | "", "clear") == 0) {
    kerryPmClearReportsAndCounters();
    out["ok"] = true;
    return sendJson(req, out);
  }
  out["ok"] = false;
  out["message"] = "Unknown action";
  return sendJson(req, out);
}

static esp_err_t apiStatsGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  kerryStatsFillJson(doc);
  doc["uploadStatus"] = uploadStatus;
  doc["ecowittStatus"] = ecowittSummary();
  return sendJson(req, doc);
}

static esp_err_t apiStatsPost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  JsonDocument out;
  if (deserializeJson(in, readBody(req, 256))) {
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }
  if (strcmp(in["action"] | "", "reset") == 0) {
    const char *err = kerryStatsReset();
    out["ok"] = err == nullptr;
    if (err) out["message"] = err;
    return sendJson(req, out);
  }
  out["ok"] = false;
  out["message"] = "Unknown action";
  return sendJson(req, out);
}

static esp_err_t flushChunk(httpd_req_t *req, const char *buf, size_t n) {
  if (!n) return ESP_OK;
  return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendJsonString(httpd_req_t *req, const char *s, size_t n) {
  char batch[256];
  size_t bi = 0;
  if (httpd_resp_sendstr_chunk(req, "\"") != ESP_OK) return ESP_FAIL;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    const char *esc = nullptr;
    char ubuf[8];
    if (c == '"') esc = "\\\"";
    else if (c == '\\') esc = "\\\\";
    else if ((unsigned char)c < 0x20) {
      snprintf(ubuf, sizeof ubuf, "\\u%04x", (unsigned)c);
      esc = ubuf;
    }
    if (esc) {
      size_t el = strlen(esc);
      if (bi + el >= sizeof batch) {
        if (flushChunk(req, batch, bi) != ESP_OK) return ESP_FAIL;
        bi = 0;
      }
      memcpy(batch + bi, esc, el);
      bi += el;
    } else {
      if (bi + 1 >= sizeof batch) {
        if (flushChunk(req, batch, bi) != ESP_OK) return ESP_FAIL;
        bi = 0;
      }
      batch[bi++] = c;
    }
  }
  if (bi && flushChunk(req, batch, bi) != ESP_OK) return ESP_FAIL;
  return httpd_resp_sendstr_chunk(req, "\"");
}

static esp_err_t apiLogGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  char q[32];
  uint32_t since = 0;
  if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(q, "since", val, sizeof val) == ESP_OK) since = (uint32_t)strtoul(val, nullptr, 10);
  }
  size_t cap = 16384;
  char *buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    cap = 4096;
    buf = (char *)malloc(cap);
  }
  if (!buf) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "Out of memory");
  }
  uint32_t lastSeq = since;
  bool truncated = false;
  size_t n = kerryLogHistoryRead(since, buf, cap, &lastSeq, &truncated);
  httpd_resp_set_type(req, "application/json");
  if (httpd_resp_sendstr_chunk(req, "{\"lines\":[") != ESP_OK) {
    free(buf);
    return ESP_FAIL;
  }
  bool first = true;
  const char *p = buf;
  while (p < buf + n) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(buf + n - p));
    if (!nl) break;
    if (!first && httpd_resp_sendstr_chunk(req, ",") != ESP_OK) {
      free(buf);
      return ESP_FAIL;
    }
    first = false;
    if (sendJsonString(req, p, (size_t)(nl - p)) != ESP_OK) {
      free(buf);
      return ESP_FAIL;
    }
    p = nl + 1;
  }
  char tail[64];
  snprintf(tail, sizeof tail, "],\"lastSeq\":%lu,\"truncated\":%s}", (unsigned long)lastSeq, truncated ? "true" : "false");
  esp_err_t err = httpd_resp_sendstr_chunk(req, tail);
  if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, nullptr);
  free(buf);
  return err;
}

void kerryWebDiagApiRegister(httpd_handle_t server) {
  httpd_uri_t getPm = {.uri = "/api/postmortem", .method = HTTP_GET, .handler = apiPostmortemGet, .user_ctx = nullptr};
  httpd_uri_t postPm = {.uri = "/api/postmortem", .method = HTTP_POST, .handler = apiPostmortemPost, .user_ctx = nullptr};
  httpd_uri_t getSt = {.uri = "/api/stats", .method = HTTP_GET, .handler = apiStatsGet, .user_ctx = nullptr};
  httpd_uri_t postSt = {.uri = "/api/stats", .method = HTTP_POST, .handler = apiStatsPost, .user_ctx = nullptr};
  httpd_uri_t getLog = {.uri = "/api/log", .method = HTTP_GET, .handler = apiLogGet, .user_ctx = nullptr};
  registerUri(server, &getPm);
  registerUri(server, &postPm);
  registerUri(server, &getSt);
  registerUri(server, &postSt);
  registerUri(server, &getLog);
}
