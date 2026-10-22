#include "web_diag_api.h"

#include "ecowitt.h"
#include "kerry_postmortem.h"
#include "kerry_shared.h"
#include "kerry_stats.h"
#include "kerry_log_history.h"
#include "kerry_web_util.h"

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

static esp_err_t apiLogGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  char q[32];
  uint32_t since = 0;
  if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(q, "since", val, sizeof val) == ESP_OK) since = (uint32_t)strtoul(val, nullptr, 10);
  }
  static char chunkBuf[16384];
  uint32_t lastSeq = since;
  bool truncated = false;
  size_t n = kerryLogHistoryRead(since, chunkBuf, sizeof chunkBuf, &lastSeq, &truncated);
  JsonDocument doc;
  doc["lastSeq"] = lastSeq;
  doc["truncated"] = truncated;
  JsonArray lines = doc["lines"].to<JsonArray>();
  const char *p = chunkBuf;
  while (p < chunkBuf + n) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(chunkBuf + n - p));
    if (!nl) break;
    lines.add(String(p, nl - p));
    p = nl + 1;
  }
  String body;
  serializeJson(doc, body);
  if (body.length() <= sizeof chunkBuf) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.c_str());
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{\"lines\":[");
  bool first = true;
  p = chunkBuf;
  while (p < chunkBuf + n) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(chunkBuf + n - p));
    if (!nl) break;
    if (!first) httpd_resp_sendstr_chunk(req, ",");
    first = false;
    httpd_resp_sendstr_chunk(req, "\"");
    httpd_resp_send_chunk(req, p, (size_t)(nl - p));
    httpd_resp_sendstr_chunk(req, "\"");
    p = nl + 1;
  }
  char tail[64];
  snprintf(tail, sizeof tail, "],\"lastSeq\":%lu,\"truncated\":%s}", (unsigned long)lastSeq, truncated ? "true" : "false");
  httpd_resp_sendstr_chunk(req, tail);
  return httpd_resp_sendstr_chunk(req, nullptr);
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
