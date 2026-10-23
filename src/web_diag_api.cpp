#include "web_diag_api.h"

#include "ecowitt.h"
#include "kerry_diag_fs.h"
#include "kerry_postmortem.h"
#include "kerry_shared.h"
#include "kerry_stats.h"
#include "kerry_log_history.h"
#include "kerry_web_util.h"

#include <esp_heap_caps.h>

#include <ArduinoJson.h>

#include <stdlib.h>
#include <string.h>

static bool queryValue(httpd_req_t *req, const char *key, char *val, size_t cap) {
  char q[96];
  if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
  return httpd_query_key_value(q, key, val, cap) == ESP_OK;
}

static uint32_t queryU32(httpd_req_t *req, const char *key, uint32_t def) {
  char val[16];
  if (!queryValue(req, key, val, sizeof val)) return def;
  return (uint32_t)strtoul(val, nullptr, 10);
}

static bool queryIs(httpd_req_t *req, const char *key, const char *want) {
  char val[16];
  return queryValue(req, key, val, sizeof val) && strcmp(val, want) == 0;
}

static char *allocPsram(size_t want, size_t fallback, size_t *cap) {
  char *buf = (char *)heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf) {
    *cap = want;
    return buf;
  }
  buf = (char *)malloc(fallback);
  *cap = buf ? fallback : 0;
  return buf;
}

static esp_err_t sendOutOfMemory(httpd_req_t *req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  return httpd_resp_sendstr(req, "Out of memory");
}

static esp_err_t sendNotFound(httpd_req_t *req, const char *message) {
  httpd_resp_set_status(req, "404 Not Found");
  JsonDocument out;
  out["ok"] = false;
  out["message"] = message;
  return sendJson(req, out);
}

static esp_err_t apiPostmortemGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc(kerryJsonPsramAllocator());
  kerryPmFillReportsJson(doc);
  return sendJson(req, doc);
}

static esp_err_t apiPostmortemReportGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  uint32_t seq = queryU32(req, "seq", 0);
  size_t cap = 0;
  char *buf = allocPsram(KERRY_DIAG_REPORT_MAX + 1, KERRY_DIAG_REPORT_MAX + 1, &cap);
  if (!buf) return sendOutOfMemory(req);
  size_t n = seq ? kerryDiagReadReport(seq, buf, cap) : 0;
  esp_err_t err;
  if (!n) {
    err = sendNotFound(req, "No report for that boot");
  } else {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, buf, n);
  }
  free(buf);
  return err;
}

static esp_err_t apiPostmortemLogGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  uint32_t seq = queryU32(req, "seq", 0);
  size_t cap = 0;
  char *buf = allocPsram(KERRY_DIAG_LOG_MAX + 1, KERRY_DIAG_LOG_MAX + 1, &cap);
  if (!buf) return sendOutOfMemory(req);
  size_t n = seq ? kerryDiagReadLog(seq, buf, cap) : 0;
  esp_err_t err;
  if (!n) {
    err = sendNotFound(req, "No saved log for that boot");
  } else {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, buf, n);
  }
  free(buf);
  return err;
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
  JsonDocument doc(kerryJsonPsramAllocator());
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

static esp_err_t sendLogJson(httpd_req_t *req, const char *buf, size_t n, const kerry_log_read_t *info) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  kerry_resp_writer_t w;
  kerryRespInit(&w, req);
  kerryRespPutStr(&w, "{\"lines\":[");
  bool first = true;
  const char *p = buf;
  while (p < buf + n && w.err == ESP_OK) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(buf + n - p));
    if (!nl) break;
    if (!first) kerryRespPutStr(&w, ",");
    first = false;
    kerryRespPutJsonString(&w, p, (size_t)(nl - p));
    p = nl + 1;
  }
  char tail[96];
  snprintf(tail, sizeof tail, "],\"lastSeq\":%lu,\"truncated\":%s,\"more\":%s}", (unsigned long)info->lastSeq,
           info->truncated ? "true" : "false", info->more ? "true" : "false");
  kerryRespPutStr(&w, tail);
  return kerryRespFinish(&w);
}

// ?since=N continues after line N; ?tail=1 (with since 0) starts from the newest lines;
// ?format=text returns plain lines with X-Log-* headers instead of JSON.
static esp_err_t apiLogGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  uint32_t since = queryU32(req, "since", 0);
  bool tail = queryIs(req, "tail", "1");
  bool text = queryIs(req, "format", "text");
  size_t cap = 0;
  char *buf = allocPsram(65536, 4096, &cap);
  if (!buf) return sendOutOfMemory(req);
  kerry_log_read_t info;
  size_t n = kerryLogHistoryRead(since, tail, buf, cap, &info);
  esp_err_t err;
  if (text) {
    char lastSeq[12];
    snprintf(lastSeq, sizeof lastSeq, "%lu", (unsigned long)info.lastSeq);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Last-Seq", lastSeq);
    httpd_resp_set_hdr(req, "X-Log-More", info.more ? "1" : "0");
    httpd_resp_set_hdr(req, "X-Log-Truncated", info.truncated ? "1" : "0");
    err = httpd_resp_send(req, buf, n);
  } else {
    err = sendLogJson(req, buf, n, &info);
  }
  free(buf);
  return err;
}

void kerryWebDiagApiRegister(httpd_handle_t server) {
  httpd_uri_t getPm = {.uri = "/api/postmortem", .method = HTTP_GET, .handler = apiPostmortemGet, .user_ctx = nullptr};
  httpd_uri_t postPm = {.uri = "/api/postmortem", .method = HTTP_POST, .handler = apiPostmortemPost, .user_ctx = nullptr};
  httpd_uri_t getSt = {.uri = "/api/stats", .method = HTTP_GET, .handler = apiStatsGet, .user_ctx = nullptr};
  httpd_uri_t postSt = {.uri = "/api/stats", .method = HTTP_POST, .handler = apiStatsPost, .user_ctx = nullptr};
  httpd_uri_t getLog = {.uri = "/api/log", .method = HTTP_GET, .handler = apiLogGet, .user_ctx = nullptr};
  httpd_uri_t getPmReport = {
      .uri = "/api/postmortem/report", .method = HTTP_GET, .handler = apiPostmortemReportGet, .user_ctx = nullptr};
  httpd_uri_t getPmLog = {
      .uri = "/api/postmortem/log", .method = HTTP_GET, .handler = apiPostmortemLogGet, .user_ctx = nullptr};
  registerUri(server, &getPm);
  registerUri(server, &postPm);
  registerUri(server, &getPmReport);
  registerUri(server, &getPmLog);
  registerUri(server, &getSt);
  registerUri(server, &postSt);
  registerUri(server, &getLog);
}
