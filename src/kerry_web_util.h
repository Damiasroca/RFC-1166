#pragma once

#include <ArduinoJson.h>
#include <WString.h>
#include <esp_http_server.h>

#include <stddef.h>

bool cookieAuth(httpd_req_t *req);
esp_err_t sendJson(httpd_req_t *req, JsonDocument &doc);
String readBody(httpd_req_t *req, size_t maxLen);
bool requireAuth(httpd_req_t *req, bool json);
void registerUri(httpd_handle_t server, httpd_uri_t *uri);

// JsonDocument pools in PSRAM so big responses leave internal RAM to Wi-Fi.
ArduinoJson::Allocator *kerryJsonPsramAllocator();

// Buffers small responses into one send; larger ones go out in 1 KB chunks.
typedef struct {
  httpd_req_t *req;
  char buf[1024];
  size_t used;
  bool chunked;
  esp_err_t err;
} kerry_resp_writer_t;

void kerryRespInit(kerry_resp_writer_t *w, httpd_req_t *req);
esp_err_t kerryRespPut(kerry_resp_writer_t *w, const void *data, size_t n);
esp_err_t kerryRespPutStr(kerry_resp_writer_t *w, const char *s);
esp_err_t kerryRespPutJsonString(kerry_resp_writer_t *w, const char *s, size_t n);
esp_err_t kerryRespFinish(kerry_resp_writer_t *w);
