#pragma once

#include <ArduinoJson.h>
#include <WString.h>
#include <esp_http_server.h>

bool cookieAuth(httpd_req_t *req);
esp_err_t sendJson(httpd_req_t *req, JsonDocument &doc);
String readBody(httpd_req_t *req, size_t maxLen);
bool requireAuth(httpd_req_t *req, bool json);
void registerUri(httpd_handle_t server, httpd_uri_t *uri);
