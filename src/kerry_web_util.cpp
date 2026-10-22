#include "kerry_web_util.h"

#include "kerry_log.h"

esp_err_t sendJson(httpd_req_t *req, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out.c_str(), out.length());
}

String readBody(httpd_req_t *req, size_t maxLen) {
  if (req->content_len <= 0 || (size_t)req->content_len > maxLen) return "";
  String body;
  body.reserve(req->content_len);
  int left = req->content_len;
  char buf[256];
  while (left > 0) {
    int chunk = left > (int)sizeof(buf) ? (int)sizeof(buf) : left;
    int n = httpd_req_recv(req, buf, chunk);
    if (n <= 0) return "";
    for (int i = 0; i < n; i++) body += buf[i];
    left -= n;
  }
  return body;
}

bool requireAuth(httpd_req_t *req, bool json) {
  if (cookieAuth(req)) return true;
  if (json) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["message"] = "Login required";
    sendJson(req, doc);
  } else {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login.html");
    httpd_resp_send(req, nullptr, 0);
  }
  return false;
}

void registerUri(httpd_handle_t server, httpd_uri_t *uri) {
  esp_err_t err = httpd_register_uri_handler(server, uri);
  if (err != ESP_OK) KERRY_LOGW("http", "route failed %s (%s)", uri->uri, esp_err_to_name(err));
}
