#include "kerry_web_util.h"

#include "kerry_log.h"

#include <esp_heap_caps.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
  }
  void deallocate(void *p) override { free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
  }
};

struct RespJsonWriter {
  kerry_resp_writer_t *w;
  size_t write(uint8_t c) { return kerryRespPut(w, &c, 1) == ESP_OK ? 1 : 0; }
  size_t write(const uint8_t *s, size_t n) { return kerryRespPut(w, s, n) == ESP_OK ? n : 0; }
};

}  // namespace

ArduinoJson::Allocator *kerryJsonPsramAllocator() {
  static PsramAllocator s_alloc;
  return &s_alloc;
}

void kerryRespInit(kerry_resp_writer_t *w, httpd_req_t *req) {
  w->req = req;
  w->used = 0;
  w->chunked = false;
  w->err = ESP_OK;
}

static void respFlush(kerry_resp_writer_t *w) {
  if (w->err != ESP_OK || !w->used) return;
  w->chunked = true;
  w->err = httpd_resp_send_chunk(w->req, w->buf, w->used);
  w->used = 0;
}

esp_err_t kerryRespPut(kerry_resp_writer_t *w, const void *data, size_t n) {
  if (w->err != ESP_OK) return w->err;
  const char *p = (const char *)data;
  while (n) {
    size_t room = sizeof w->buf - w->used;
    if (!room) {
      respFlush(w);
      if (w->err != ESP_OK) return w->err;
      room = sizeof w->buf;
    }
    size_t take = n < room ? n : room;
    memcpy(w->buf + w->used, p, take);
    w->used += take;
    p += take;
    n -= take;
  }
  return ESP_OK;
}

esp_err_t kerryRespPutStr(kerry_resp_writer_t *w, const char *s) { return kerryRespPut(w, s, strlen(s)); }

esp_err_t kerryRespPutJsonString(kerry_resp_writer_t *w, const char *s, size_t n) {
  if (kerryRespPutStr(w, "\"") != ESP_OK) return w->err;
  size_t run = 0;
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
    if (!esc) continue;
    if (i > run && kerryRespPut(w, s + run, i - run) != ESP_OK) return w->err;
    if (kerryRespPutStr(w, esc) != ESP_OK) return w->err;
    run = i + 1;
  }
  if (n > run && kerryRespPut(w, s + run, n - run) != ESP_OK) return w->err;
  return kerryRespPutStr(w, "\"");
}

esp_err_t kerryRespFinish(kerry_resp_writer_t *w) {
  if (w->err != ESP_OK) return w->err;
  if (!w->chunked) return httpd_resp_send(w->req, w->buf, w->used);
  respFlush(w);
  if (w->err != ESP_OK) return w->err;
  return httpd_resp_send_chunk(w->req, nullptr, 0);
}

esp_err_t sendJson(httpd_req_t *req, JsonDocument &doc) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  kerry_resp_writer_t w;
  kerryRespInit(&w, req);
  RespJsonWriter jw{&w};
  serializeJson(doc, jw);
  return kerryRespFinish(&w);
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
