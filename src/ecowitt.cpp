#include "ecowitt.h"

#include "secrets.h"
#include "kerry_log.h"
#include "kerry_postmortem.h"
#include "kerry_stats.h"
#include "kerry_shared.h"
#include "kerry_time.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/md.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *ECO_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36";
static const char *ECO_WEB_VER = "v1.228_03_11";
static const char *STATION_TYPE = KERRY_ECOWITT_STATION_TYPE;
static const char *MODEL = KERRY_ECOWITT_MODEL;
static const char *SUGGEST_NAME = KERRY_ECOWITT_SUGGEST_NAME;
static const char *BOUNDARY = "----DataPackageBoundary";

static const int JOB_NONE = 0;
static const int JOB_LOGIN = 1;
static const int JOB_REGISTER = 2;
static const int JOB_LOGOUT = 3;

static const int PH_IDLE = 0;
static const int PH_ARM = 1;
static const int PH_WAIT = 2;
static const int PH_RETRY = 3;

static const int PARSE_USER = 1;
static const int PARSE_LIST = 2;
static const int PARSE_SAVE = 3;

struct EcoCookie {
  char name[24];
  char value[128];
};

struct EcoDev {
  char id[17];
  char name[32];
  char mac[24];
  char model[32];
  char zone[40];
  char online[48];
  bool mine;
};

struct Kv {
  const char *k;
  const char *v;
};

struct HttpAcc {
  char *buf;
  size_t cap;
  size_t n;
  bool trunc;
};

static SemaphoreHandle_t s_mu = nullptr;
static TaskHandle_t s_task = nullptr;
static char *s_httpBody = nullptr;
static char *s_scratch = nullptr;

static char s_acct[81];
static char s_pwd[65];
static char s_uid[17];
static char s_nick[49];
static char s_devid[17];
static bool s_en = false;
static uint8_t s_iv = 1;
static bool s_seen = false;
static bool s_checked = true;
static bool s_working = false;
static bool s_now = false;
static int s_job = JOB_NONE;
static char s_jobAcct[81];
static char s_jobPwd[65];
static char s_jobName[32];
static char s_msg[128];
static char s_checkErr[128];
static char s_listErr[128];
static char s_uploadStatus[96];
static char s_schedule[96];
static char s_macUp[18];
static char s_macLo[18];
static EcoCookie s_cks[8];
static int s_nck = 0;
static EcoDev s_devs[20];
static int s_ndev = 0;
static uint8_t s_reupload = 0;
static uint8_t s_host = 0;
static uint8_t s_armedSlot = 0xff;
static uint8_t s_phase = PH_IDLE;
static uint32_t s_deadline = 0;
static uint32_t s_sessionDue = 0;
static bool s_grabCookies = false;

static char s_signStore[16][192];
static char s_signRaw[2048];
static char s_cookieHdr[1400];

static bool lockWait(TickType_t ticks) {
  if (!s_mu) return false;
  return xSemaphoreTake(s_mu, ticks) == pdTRUE;
}

static void unlock() { xSemaphoreGive(s_mu); }

static void setText(char *dst, size_t n, const char *src) {
  if (!dst || n == 0) return;
  snprintf(dst, n, "%s", src ? src : "");
}

static void trim(char *s) {
  if (!s) return;
  char *start = s;
  while (*start == ' ') start++;
  if (start != s) memmove(s, start, strlen(start) + 1);
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}

static void loadMac() {
  uint8_t m[6] = {};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(s_macUp, sizeof s_macUp, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  snprintf(s_macLo, sizeof s_macLo, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static bool md5Hex(const uint8_t *data, size_t n, char out[33]) {
  uint8_t dig[16];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  if (!info || mbedtls_md(info, data, n, dig) != 0) return false;
  for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02X", dig[i]);
  out[32] = 0;
  return true;
}

static bool encUri(const char *in, char *out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  if (!in) in = "";
  for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
    unsigned char c = *p;
    bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                 c == '_' || c == '.';
    if (c == ' ') {
      if (n + 1 >= cap) return false;
      out[n++] = '+';
    } else if (plain) {
      if (n + 1 >= cap) return false;
      out[n++] = (char)c;
    } else {
      if (n + 3 >= cap) return false;
      out[n++] = '%';
      out[n++] = hex[c >> 4];
      out[n++] = hex[c & 0x0f];
    }
  }
  if (n >= cap) return false;
  out[n] = 0;
  return true;
}

static int partCmp(const void *a, const void *b) {
  const char *sa = *(const char *const *)a;
  const char *sb = *(const char *const *)b;
  return strcmp(sa, sb);
}

static bool ecoSign(const Kv *items, int n, char out[33]) {
  if (n <= 0 || n > 16) return false;
  const char *parts[16];
  for (int i = 0; i < n; i++) {
    char enc[160];
    if (!encUri(items[i].v ? items[i].v : "", enc, sizeof enc)) return false;
    if (snprintf(s_signStore[i], sizeof s_signStore[i], "%s=%s", items[i].k, enc) >= (int)sizeof s_signStore[i])
      return false;
    parts[i] = s_signStore[i];
  }
  qsort(parts, (size_t)n, sizeof parts[0], partCmp);
  size_t len = 0;
  for (int i = 0; i < n; i++) {
    size_t partLen = strlen(parts[i]);
    if (len + (i ? 1 : 0) + partLen + 12 >= sizeof s_signRaw) return false;
    if (i) s_signRaw[len++] = '&';
    memcpy(s_signRaw + len, parts[i], partLen);
    len += partLen;
  }
  memcpy(s_signRaw + len, "@ecowittnet", 11);
  len += 11;
  s_signRaw[len] = 0;
  return md5Hex((const uint8_t *)s_signRaw, len, out);
}

static bool selfCheck() {
  char enc[160];
  char sign[33];
  bool ok = true;
  const char *addr = "40.712800\xC2\xB0 N, 74.006000\xC2\xB0 W";
  struct {
    const char *in;
    const char *want;
  } vectors[] = {
      {"user@example.com", "user%40example.com"},
      {"p a/ss", "p+a%2Fss"},
      {"Sky Cam", "Sky+Cam"},
      {"America/New_York", "America%2FNew_York"},
      {"AA:BB:CC:DD:EE:FF", "AA%3ABB%3ACC%3ADD%3AEE%3AFF"},
      {addr, "40.712800%C2%B0+N%2C+74.006000%C2%B0+W"},
  };
  for (const auto &v : vectors) {
    if (!encUri(v.in, enc, sizeof enc) || strcmp(enc, v.want) != 0) {
      KERRY_LOGE("ecowitt", "encoder mismatch, got %s", enc);
      ok = false;
    }
  }
  Kv listKv[] = {{"user_id", "12345"}, {"time", "1710000000"}};
  if (!ecoSign(listKv, 2, sign) || strcmp(sign, "A257857B33DDF8AD732BD1B3429C8445") != 0) {
    KERRY_LOGE("ecowitt", "device-list sign mismatch, got %s", sign);
    ok = false;
  }
  Kv saveKv[] = {
      {"name", "Sky Cam"},
      {"addr", addr},
      {"longitude", "-74.006000"},
      {"latitude", "40.712800"},
      {"type", "2"},
      {"date_zone_id", "America/New_York"},
      {"is_summer_time", "1"},
      {"mac", "AA:BB:CC:DD:EE:FF"},
      {"imei", ""},
      {"is_public", "1"},
      {"tfp", "1"},
      {"user_id", "12345"},
      {"device_id", ""},
      {"time", "1710000000"},
  };
  if (!ecoSign(saveKv, 14, sign) || strcmp(sign, "6A47382C9BAD785D00BDD3623850D45A") != 0) {
    KERRY_LOGE("ecowitt", "saveDevice sign mismatch, got %s raw %s", sign, s_signRaw);
    ok = false;
  }
  if (!md5Hex((const uint8_t *)"AA:BB:CC:DD:EE:FF", 17, sign) || strcmp(sign, "6393CFC4AB6D416690B21278957AEAB2") != 0) {
    KERRY_LOGE("ecowitt", "passkey mismatch, got %s", sign);
    ok = false;
  }
  const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
  // MD5(FF D8 FF D9). The writeup lists a different digest; this is the real one.
  if (!md5Hex(jpeg, sizeof jpeg, sign) || strcmp(sign, "F5EE0EC808CB0DA7058C84210B3FBB87") != 0) {
    KERRY_LOGE("ecowitt", "jpeg md5 mismatch, got %s", sign);
    ok = false;
  }
  return ok;
}

static void ckClear() { s_nck = 0; }

static void ckPut(const char *setCookie) {
  char pair[160];
  size_t i = 0;
  if (!setCookie) return;
  while (setCookie[i] && setCookie[i] != ';' && i + 1 < sizeof pair) {
    pair[i] = setCookie[i];
    i++;
  }
  pair[i] = 0;
  char *eq = strchr(pair, '=');
  if (!eq || eq == pair) return;
  *eq = 0;
  if (strlen(pair) > 23 || strlen(eq + 1) > 127) {
    KERRY_LOGW("ecowitt", "cookie skipped name=%u value=%u", (unsigned)strlen(pair), (unsigned)strlen(eq + 1));
    return;
  }
  for (int c = 0; c < s_nck; c++) {
    if (strcmp(s_cks[c].name, pair) == 0) {
      snprintf(s_cks[c].value, sizeof s_cks[c].value, "%s", eq + 1);
      return;
    }
  }
  if (s_nck >= 8) return;
  snprintf(s_cks[s_nck].name, sizeof s_cks[s_nck].name, "%s", pair);
  snprintf(s_cks[s_nck].value, sizeof s_cks[s_nck].value, "%s", eq + 1);
  s_nck++;
}

static void ckHeader(char *out, size_t n) {
  size_t len = 0;
  out[0] = 0;
  for (int i = 0; i < s_nck; i++) {
    int w = snprintf(out + len, n - len, "%s%s=%s", len ? "; " : "", s_cks[i].name, s_cks[i].value);
    if (w < 0 || (size_t)w >= n - len) {
      out[0] = 0;
      return;
    }
    len += (size_t)w;
  }
}

static esp_err_t httpEvt(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (s_grabCookies && evt->header_key && evt->header_value && strcasecmp(evt->header_key, "Set-Cookie") == 0)
      ckPut(evt->header_value);
  } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data_len > 0) {
    HttpAcc *acc = (HttpAcc *)evt->user_data;
    if (!acc->buf || acc->n + 1 >= acc->cap) {
      acc->trunc = true;
      return ESP_OK;
    }
    size_t room = acc->cap - 1 - acc->n;
    size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    if ((size_t)evt->data_len > room) acc->trunc = true;
    if (take) {
      memcpy(acc->buf + acc->n, evt->data, take);
      acc->n += take;
      acc->buf[acc->n] = 0;
    }
  }
  return ESP_OK;
}

static void buildFilter(int kind, JsonDocument &filter) {
  filter["code"] = true;
  filter["errcode"] = true;
  filter["errmsg"] = true;
  filter["msg"] = true;
  if (kind == PARSE_USER) {
    filter["id"] = true;
    filter["uid"] = true;
    filter["nickname"] = true;
  } else if (kind == PARSE_SAVE) {
    filter["data"]["id"] = true;
  } else if (kind == PARSE_LIST) {
    filter["data"]["rows"][0]["id"] = true;
    filter["data"]["rows"][0]["mac"] = true;
    filter["data"]["rows"][0]["name"] = true;
    filter["data"]["rows"][0]["model"] = true;
    filter["data"]["rows"][0]["date_zone_id"] = true;
    filter["data"]["rows"][0]["re_time_text"] = true;
  }
}

static bool apiCode(JsonDocument &doc, int *code) {
  JsonVariant c = doc["code"];
  if (c.is<JsonInteger>()) {
    *code = (int)c.as<JsonInteger>();
    return true;
  }
  if (c.is<double>()) {
    *code = (int)c.as<double>();
    return true;
  }
  JsonVariant e = doc["errcode"];
  if (e.is<const char *>()) {
    *code = atoi(e.as<const char *>());
    return true;
  }
  if (e.is<JsonInteger>()) {
    *code = (int)e.as<JsonInteger>();
    return true;
  }
  if (e.is<double>()) {
    *code = (int)e.as<double>();
    return true;
  }
  return false;
}

static void apiMsg(JsonDocument &doc, char *dst, size_t n, const char *fallback) {
  JsonVariant em = doc["errmsg"];
  if (em.is<const char *>() && em.as<const char *>()[0]) {
    snprintf(dst, n, "%s", em.as<const char *>());
    return;
  }
  JsonVariant msg = doc["msg"];
  if (msg.is<const char *>() && msg.as<const char *>()[0]) {
    snprintf(dst, n, "%s", msg.as<const char *>());
    return;
  }
  snprintf(dst, n, "%s", fallback ? fallback : "");
}

static void readId(JsonVariant v, char *dst, size_t n) {
  dst[0] = 0;
  if (v.is<const char *>()) {
    snprintf(dst, n, "%s", v.as<const char *>());
    return;
  }
  if (v.is<JsonInteger>()) {
    snprintf(dst, n, "%lld", (long long)v.as<JsonInteger>());
    return;
  }
  if (v.is<double>()) snprintf(dst, n, "%.0f", v.as<double>());
}

static bool httpJson(esp_http_client_method_t method, const char *url, const char *contentType, const char *referer,
                     const char *body, bool api, int kind, JsonDocument &doc, char *err, size_t errn) {
  if (!s_httpBody) {
    snprintf(err, errn, "Out of memory");
    return false;
  }
  HttpAcc acc = {};
  acc.buf = s_httpBody;
  acc.cap = 16384;
  s_httpBody[0] = 0;
  s_grabCookies = true;
  ckHeader(s_cookieHdr, sizeof s_cookieHdr);
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = method;
  cfg.timeout_ms = 20000;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;
  cfg.event_handler = httpEvt;
  cfg.user_data = &acc;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  if (!cli) {
    snprintf(err, errn, "HTTP client failed");
    return false;
  }
  esp_http_client_set_header(cli, "Accept", "application/json, text/plain, */*");
  esp_http_client_set_header(cli, "User-Agent", ECO_UA);
  esp_http_client_set_header(cli, "Origin", "https://www.ecowitt.net");
  if (referer) esp_http_client_set_header(cli, "Referer", referer);
  if (contentType) esp_http_client_set_header(cli, "Content-Type", contentType);
  if (api) {
    esp_http_client_set_header(cli, "accept-ecowittlang", "en");
    esp_http_client_set_header(cli, "web-version", ECO_WEB_VER);
  }
  if (s_cookieHdr[0]) esp_http_client_set_header(cli, "Cookie", s_cookieHdr);
  if (body) esp_http_client_set_post_field(cli, body, (int)strlen(body));
  esp_err_t rc = esp_http_client_perform(cli);
  int status = esp_http_client_get_status_code(cli);
  esp_http_client_cleanup(cli);
  s_grabCookies = false;
  if (rc != ESP_OK) {
    snprintf(err, errn, "Ecowitt unreachable (%s)", esp_err_to_name(rc));
    return false;
  }
  if (status < 200 || status > 299) {
    snprintf(err, errn, "Ecowitt HTTP %d", status);
    return false;
  }
  if (acc.trunc) {
    snprintf(err, errn, "Ecowitt response too large");
    return false;
  }
  JsonDocument filter;
  buildFilter(kind, filter);
  DeserializationError jerr = deserializeJson(doc, s_httpBody, DeserializationOption::Filter(filter));
  if (jerr) {
    snprintf(err, errn, jerr == DeserializationError::NoMemory ? "Out of memory" : "Bad response from Ecowitt");
    return false;
  }
  return true;
}

static bool macEq(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
    if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
    if (ca != cb) return false;
  }
  return *a == 0 && *b == 0;
}

static bool staUp() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

static bool netUp() { return staUp() && portalOk; }

static const char *netReadyMsg() {
  if (WiFi.status() != WL_CONNECTED) return "Connect the camera to the router first";
  if (!portalOk) return "Internet is not open yet";
  time_t now = 0;
  time(&now);
  if (now < 1600000000) return "Clock is not set yet";
  return nullptr;
}

static void saveCredsLocked() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putString("eco_acct", s_acct);
  prefs.putString("eco_pwd", s_pwd);
  prefs.putString("eco_uid", s_uid);
  prefs.putString("eco_nick", s_nick);
  prefs.putString("eco_devid", s_devid);
  prefs.end();
}

static void saveSchedLocked() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putBool("eco_en", s_en);
  prefs.putUChar("eco_iv", s_iv);
  prefs.end();
}

static void loadCreds() {
  Preferences prefs;
  prefs.begin("kerry", true);
  String acct = prefs.getString("eco_acct", "");
  String pwd = prefs.getString("eco_pwd", "");
  String uid = prefs.getString("eco_uid", "");
  String nick = prefs.getString("eco_nick", "");
  String devid = prefs.getString("eco_devid", "");
  s_en = prefs.getBool("eco_en", false);
  s_iv = prefs.getUChar("eco_iv", 1);
  prefs.end();
  setText(s_acct, sizeof s_acct, acct.c_str());
  setText(s_pwd, sizeof s_pwd, pwd.c_str());
  setText(s_uid, sizeof s_uid, uid.c_str());
  setText(s_nick, sizeof s_nick, nick.c_str());
  setText(s_devid, sizeof s_devid, devid.c_str());
  if (s_iv > 5) s_iv = 1;
}

static void takeUser(JsonDocument &doc, char *id, size_t idn, char *nick, size_t nickn, bool *haveNick) {
  id[0] = 0;
  nick[0] = 0;
  *haveNick = false;
  if (!doc["id"].isNull()) readId(doc["id"], id, idn);
  if (!id[0]) readId(doc["uid"], id, idn);
  if (doc["nickname"].is<const char *>()) {
    snprintf(nick, nickn, "%s", doc["nickname"].as<const char *>());
    *haveNick = true;
  }
}

static bool doLogin(const char *acct, const char *pwd, char *err, size_t errn) {
  if (!s_scratch) {
    snprintf(err, errn, "Out of memory");
    return false;
  }
  char *eacct = s_scratch;
  char *epwd = s_scratch + 256;
  char *body = s_scratch + 512;
  if (!encUri(acct, eacct, 256) || !encUri(pwd, epwd, 256)) {
    snprintf(err, errn, "Account or password is too long");
    return false;
  }
  snprintf(body, 640, "account=%s&password=%s&authorize=", eacct, epwd);
  ckClear();
  JsonDocument doc;
  if (!httpJson(HTTP_METHOD_POST, "https://www.ecowitt.net/user/site/login", "application/x-www-form-urlencoded",
                "https://www.ecowitt.net/home/login", body, false, PARSE_USER, doc, err, errn))
    return false;
  int code = 0;
  if (!apiCode(doc, &code) || code != 0) {
    apiMsg(doc, err, errn, "Login failed");
    return false;
  }
  char id[17], nick[49];
  bool haveNick = false;
  takeUser(doc, id, sizeof id, nick, sizeof nick, &haveNick);
  if (!id[0]) {
    snprintf(err, errn, "Login did not return a user id");
    return false;
  }
  if (!lockWait(portMAX_DELAY)) return false;
  setText(s_uid, sizeof s_uid, id);
  if (haveNick) setText(s_nick, sizeof s_nick, nick);
  unlock();
  KERRY_LOGI("ecowitt", "login uid=%s", id);
  return true;
}

static bool fetchUser(char *err, size_t errn) {
  JsonDocument doc;
  if (!httpJson(HTTP_METHOD_POST, "https://www.ecowitt.net/user/site/get_user_info", "application/x-www-form-urlencoded",
                "https://www.ecowitt.net/home/index", "", false, PARSE_USER, doc, err, errn))
    return false;
  int code = 0;
  if (!apiCode(doc, &code) || code != 0) {
    apiMsg(doc, err, errn, "Could not read the account");
    return false;
  }
  char id[17], nick[49];
  bool haveNick = false;
  takeUser(doc, id, sizeof id, nick, sizeof nick, &haveNick);
  if (!lockWait(portMAX_DELAY)) return false;
  if (id[0]) setText(s_uid, sizeof s_uid, id);
  if (haveNick) setText(s_nick, sizeof s_nick, nick);
  unlock();
  return s_uid[0] != 0;
}

static bool fetchDevices(char *err, size_t errn) {
  char uid[17], tbuf[24], sig[33], url[256];
  if (!lockWait(portMAX_DELAY)) return false;
  setText(uid, sizeof uid, s_uid);
  unlock();
  if (!uid[0]) {
    snprintf(err, errn, "Log in to Ecowitt first");
    return false;
  }
  time_t now = time(nullptr);
  snprintf(tbuf, sizeof tbuf, "%lld", (long long)now);
  Kv kv[] = {{"user_id", uid}, {"time", tbuf}};
  if (!ecoSign(kv, 2, sig)) {
    snprintf(err, errn, "Could not sign the device list");
    return false;
  }
  snprintf(url, sizeof url, "https://www.ecowitt.net/api/web/v1/device/getDeviceList?user_id=%s&time=%s&sign=%s", uid,
           tbuf, sig);
  JsonDocument doc;
  if (!httpJson(HTTP_METHOD_GET, url, nullptr, "https://www.ecowitt.net/home/manage", nullptr, true, PARSE_LIST, doc,
                err, errn))
    return false;
  int code = 0;
  if (!apiCode(doc, &code) || code != 0) {
    apiMsg(doc, err, errn, "Could not list devices");
    return false;
  }
  if (!lockWait(portMAX_DELAY)) return false;
  s_ndev = 0;
  bool found = false;
  char lastId[17];
  lastId[0] = 0;
  JsonArray rows = doc["data"]["rows"].as<JsonArray>();
  for (JsonObject row : rows) {
    char id[17], mac[24];
    readId(row["id"], id, sizeof id);
    mac[0] = 0;
    if (row["mac"].is<const char *>()) snprintf(mac, sizeof mac, "%s", row["mac"].as<const char *>());
    bool mine = macEq(mac, s_macUp);
    if (mine) {
      setText(lastId, sizeof lastId, id);
      found = true;
    }
    if (s_ndev >= 20) continue;
    EcoDev *d = &s_devs[s_ndev++];
    memset(d, 0, sizeof *d);
    setText(d->id, sizeof d->id, id);
    setText(d->mac, sizeof d->mac, mac);
    if (row["name"].is<const char *>()) setText(d->name, sizeof d->name, row["name"].as<const char *>());
    if (row["model"].is<const char *>()) setText(d->model, sizeof d->model, row["model"].as<const char *>());
    if (row["date_zone_id"].is<const char *>())
      setText(d->zone, sizeof d->zone, row["date_zone_id"].as<const char *>());
    if (row["re_time_text"].is<const char *>())
      setText(d->online, sizeof d->online, row["re_time_text"].as<const char *>());
    d->mine = mine;
  }
  if (found) setText(s_devid, sizeof s_devid, lastId);
  else s_devid[0] = 0;
  s_seen = found;
  unlock();
  return true;
}

static bool ensureSession(char *err, size_t errn) {
  char acct[81], pwd[65];
  int nck = 0;
  if (!lockWait(portMAX_DELAY)) return false;
  setText(acct, sizeof acct, s_acct);
  setText(pwd, sizeof pwd, s_pwd);
  nck = s_nck;
  unlock();
  if (!acct[0] || !pwd[0]) {
    snprintf(err, errn, "Log in to Ecowitt first");
    return false;
  }
  if (nck > 0 && fetchUser(err, errn)) {
    memset(pwd, 0, sizeof pwd);
    return true;
  }
  bool ok = doLogin(acct, pwd, err, errn);
  memset(pwd, 0, sizeof pwd);
  if (!ok) return false;
  fetchUser(err, errn);
  return true;
}

static bool nameOk(const char *name) {
  if (!name || !name[0] || strlen(name) > 31) return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == ' ' ||
        *p == '-' || *p == '_' || *p == '.')
      continue;
    return false;
  }
  return true;
}

static void finishMsg(const char *msg, const char *listErr) {
  if (!lockWait(portMAX_DELAY)) return;
  setText(s_msg, sizeof s_msg, msg ? msg : "");
  setText(s_listErr, sizeof s_listErr, listErr ? listErr : "");
  unlock();
}

static void beginAccountNet() {
  while (!kerryTryBeginEcoNet()) vTaskDelay(pdMS_TO_TICKS(200));
  kerryWdtLenient();
}

static void endAccountNet() {
  kerryWdtRestore();
  kerryEndEcoNet();
}

static void sessionAgain(bool ok) {
  s_sessionDue = millis() + (ok ? 30UL * 60UL * 1000UL : 3UL * 60UL * 1000UL);
}

static void runLogin(const char *acctIn, const char *pwdIn) {
  sessionAgain(false);
  char acct[81], pwd[65], savedAcct[81], savedPwd[65];
  if (!lockWait(portMAX_DELAY)) return;
  s_checked = true;
  s_checkErr[0] = 0;
  s_listErr[0] = 0;
  s_msg[0] = 0;
  setText(savedAcct, sizeof savedAcct, s_acct);
  setText(savedPwd, sizeof savedPwd, s_pwd);
  unlock();

  setText(acct, sizeof acct, acctIn[0] ? acctIn : savedAcct);
  pwd[0] = 0;
  if (pwdIn[0]) setText(pwd, sizeof pwd, pwdIn);
  else if (savedAcct[0] && strcmp(acct, savedAcct) == 0) setText(pwd, sizeof pwd, savedPwd);

  const char *msg = nullptr;
  char list[128];
  list[0] = 0;
  if (!acct[0] || !pwd[0]) {
    msg = acct[0] ? "Enter the Ecowitt password" : "Enter the Ecowitt account and password";
  } else if (strlen(acct) > 80 || strlen(pwd) > 64) {
    msg = "Account or password is too long";
  } else if (netReadyMsg()) {
    msg = netReadyMsg();
  } else {
    beginAccountNet();
    char err[128];
    bool ok = doLogin(acct, pwd, err, sizeof err);
    if (!ok) {
      endAccountNet();
      msg = nullptr;
      finishMsg(err, "");
      memset(pwd, 0, sizeof pwd);
      memset(savedPwd, 0, sizeof savedPwd);
      return;
    }
    if (!lockWait(portMAX_DELAY)) {
      endAccountNet();
      memset(pwd, 0, sizeof pwd);
      memset(savedPwd, 0, sizeof savedPwd);
      return;
    }
    if (savedAcct[0] && strcmp(savedAcct, acct) != 0) {
      s_devid[0] = 0;
      s_seen = false;
      s_ndev = 0;
    }
    setText(s_acct, sizeof s_acct, acct);
    setText(s_pwd, sizeof s_pwd, pwd);
    saveCredsLocked();
    unlock();
    memset(pwd, 0, sizeof pwd);
    memset(savedPwd, 0, sizeof savedPwd);
    fetchUser(err, sizeof err);
    bool listed = fetchDevices(err, sizeof err);
    endAccountNet();
    if (listed) {
      if (lockWait(portMAX_DELAY)) {
        s_listErr[0] = 0;
        saveCredsLocked();
        unlock();
      }
      finishMsg("Logged in", "");
    } else {
      finishMsg("Logged in", err);
    }
    sessionAgain(true);
    return;
  }
  memset(pwd, 0, sizeof pwd);
  memset(savedPwd, 0, sizeof savedPwd);
  if (msg) finishMsg(msg, list);
}

static void runRegister(const char *nameIn) {
  char name[32];
  setText(name, sizeof name, nameIn);
  trim(name);
  if (!lockWait(portMAX_DELAY)) return;
  s_checked = true;
  s_checkErr[0] = 0;
  s_listErr[0] = 0;
  s_msg[0] = 0;
  unlock();
  if (!nameOk(name)) {
    finishMsg("Device name: letters, numbers, space, . _ -", "");
    return;
  }
  const char *net = netReadyMsg();
  if (net) {
    finishMsg(net, "");
    return;
  }
  double lat = 0, lon = 0;
  char iana[40];
  if (!kerryTimeGetLocation(&lat, &lon, iana, sizeof iana)) {
    finishMsg("No GeoPulse position or time zone yet", "");
    return;
  }
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
    finishMsg("Saved position is out of range", "");
    return;
  }

  char err[128];
  beginAccountNet();
  if (!ensureSession(err, sizeof err)) {
    endAccountNet();
    finishMsg(err, "");
    return;
  }
  if (!fetchDevices(err, sizeof err)) {
    endAccountNet();
    finishMsg(err, "");
    return;
  }
  bool already = false;
  if (lockWait(portMAX_DELAY)) {
    already = s_seen && s_devid[0];
    if (already) saveCredsLocked();
    unlock();
  }
  if (already) {
    endAccountNet();
    finishMsg("This camera is already on the Ecowitt account", "");
    return;
  }

  char latS[24], lonS[24], addr[80], summer[2], tbuf[24], sig[33], uid[17];
  if (!s_scratch) {
    endAccountNet();
    finishMsg("Out of memory", "");
    return;
  }
  char *body = s_scratch;
  snprintf(latS, sizeof latS, "%.6f", lat);
  snprintf(lonS, sizeof lonS, "%.6f", lon);
  snprintf(addr, sizeof addr, "%.6f\xC2\xB0 %c, %.6f\xC2\xB0 %c", lat < 0 ? -lat : lat, lat < 0 ? 'S' : 'N',
           lon < 0 ? -lon : lon, lon < 0 ? 'W' : 'E');
  time_t now = time(nullptr);
  struct tm tm;
  summer[0] = '0';
  summer[1] = 0;
  if (localtime_r(&now, &tm) && tm.tm_isdst > 0) summer[0] = '1';
  snprintf(tbuf, sizeof tbuf, "%lld", (long long)now);
  if (!lockWait(portMAX_DELAY)) {
    endAccountNet();
    return;
  }
  setText(uid, sizeof uid, s_uid);
  unlock();
  Kv kv[] = {
      {"name", name},
      {"addr", addr},
      {"longitude", lonS},
      {"latitude", latS},
      {"type", "2"},
      {"date_zone_id", iana},
      {"is_summer_time", summer},
      {"mac", s_macUp},
      {"imei", ""},
      {"is_public", "1"},
      {"tfp", "1"},
      {"user_id", uid},
      {"device_id", ""},
      {"time", tbuf},
  };
  if (!ecoSign(kv, 14, sig)) {
    endAccountNet();
    finishMsg("Could not sign the registration", "");
    return;
  }
  int wrote = snprintf(body, 1024,
                       "{\"name\":\"%s\",\"addr\":\"%s\",\"longitude\":\"%s\",\"latitude\":\"%s\","
                       "\"type\":2,\"date_zone_id\":\"%s\",\"is_summer_time\":\"%s\","
                       "\"mac\":\"%s\",\"imei\":\"\",\"is_public\":\"1\",\"tfp\":\"1\","
                       "\"user_id\":\"%s\",\"device_id\":\"\",\"time\":%s,\"sign\":\"%s\"}",
                       name, addr, lonS, latS, iana, summer, s_macUp, uid, tbuf, sig);
  if (wrote < 0 || wrote >= 1024) {
    endAccountNet();
    finishMsg("Could not build the registration", "");
    return;
  }
  KERRY_LOGI("ecowitt", "register name=%s mac=%s tz=%s", name, s_macUp, iana);
  JsonDocument saved;
  if (!httpJson(HTTP_METHOD_POST, "https://www.ecowitt.net/api/web/v1/device/saveDevice", "application/json",
                "https://www.ecowitt.net/home/manage", body, true, PARSE_SAVE, saved, err, sizeof err)) {
    endAccountNet();
    finishMsg(err, "");
    return;
  }
  int code = 0;
  if (!apiCode(saved, &code) || code != 0) {
    apiMsg(saved, err, sizeof err, "Registration failed");
    endAccountNet();
    finishMsg(err, "");
    return;
  }
  char newId[17];
  newId[0] = 0;
  readId(saved["data"]["id"], newId, sizeof newId);
  if (newId[0] && lockWait(portMAX_DELAY)) {
    setText(s_devid, sizeof s_devid, newId);
    unlock();
  }
  bool listed = fetchDevices(err, sizeof err);
  endAccountNet();
  if (lockWait(portMAX_DELAY)) {
    saveCredsLocked();
    unlock();
  }
  if (!listed) finishMsg("Camera registered, but the device list could not be checked", err);
  else if (s_seen) finishMsg("Camera registered", "");
  else finishMsg("Registered, but this camera is not in the device list yet", "");
}

static void runLogout() {
  if (!lockWait(portMAX_DELAY)) return;
  s_checked = true;
  s_acct[0] = 0;
  s_pwd[0] = 0;
  s_uid[0] = 0;
  s_nick[0] = 0;
  s_devid[0] = 0;
  s_seen = false;
  s_ndev = 0;
  s_nck = 0;
  s_checkErr[0] = 0;
  s_listErr[0] = 0;
  memset(s_cks, 0, sizeof s_cks);
  setText(s_msg, sizeof s_msg, "Logged out");
  saveCredsLocked();
  unlock();
}

static int ecoErrcode(const char *resp) {
  const char *p = strstr(resp, "errcode");
  if (!p) return -1;
  p += 7;
  while (*p == '"' || *p == ':' || *p == ' ') p++;
  return atoi(p);
}

static bool appendMem(uint8_t *dst, size_t cap, size_t *used, const void *src, size_t n) {
  if (*used + n > cap) return false;
  memcpy(dst + *used, src, n);
  *used += n;
  return true;
}

static bool appendStr(uint8_t *dst, size_t cap, size_t *used, const char *s) {
  return appendMem(dst, cap, used, s, strlen(s));
}

static bool appendPart(uint8_t *dst, size_t cap, size_t *used, const char *name, const char *val) {
  char line[192];
  int wrote = snprintf(line, sizeof line,
                       "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", BOUNDARY, name, val);
  if (wrote < 0 || wrote >= (int)sizeof line) return false;
  return appendStr(dst, cap, used, line);
}

static bool postOne(const char *url, const uint8_t *body, size_t bodyN, char *err, size_t errn) {
  if (!s_scratch) {
    snprintf(err, errn, "Out of memory");
    return false;
  }
  char *resp = s_scratch;
  HttpAcc acc = {};
  acc.buf = resp;
  acc.cap = 768;
  resp[0] = 0;
  s_grabCookies = false;
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 30000;
  cfg.event_handler = httpEvt;
  cfg.user_data = &acc;
  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  if (!cli) {
    snprintf(err, errn, "Last Ecowitt upload failed, no connection");
    return false;
  }
  esp_http_client_set_header(cli, "Content-Type", "multipart/form-data; boundary=----DataPackageBoundary");
  esp_http_client_set_header(cli, "Accept", "*/*");
  esp_http_client_set_post_field(cli, (const char *)body, (int)bodyN);
  esp_err_t rc = esp_http_client_perform(cli);
  int status = esp_http_client_get_status_code(cli);
  esp_http_client_cleanup(cli);
  if (rc != ESP_OK || status < 200 || status > 299) {
    if (rc != ESP_OK) snprintf(err, errn, "Last Ecowitt upload failed, no connection");
    else snprintf(err, errn, "Last Ecowitt upload failed (HTTP %d)", status);
    KERRY_LOGW("ecowitt", "POST failed %s (%s, HTTP %d)", url, esp_err_to_name(rc), status);
    return false;
  }
  int code = ecoErrcode(resp);
  if (code > 0) {
    snprintf(err, errn, "Ecowitt rejected the picture (%d)", code);
    KERRY_LOGW("ecowitt", "errcode %d from %s", code, url);
    return false;
  }
  snprintf(err, errn, "Last Ecowitt upload OK");
  KERRY_LOGI("ecowitt", "upload ok %s", url);
  return true;
}

static bool postJpeg(const uint8_t *jpg, size_t jpgN, int intervalMin, char *err, size_t errn) {
  time_t now = 0;
  time(&now);
  if (now < 1600000000) {
    KERRY_LOGW("ecowitt", "clock not synced, sending epoch");
    now = 0;
  }
  struct tm tm;
  char dateutc[32];
  if (!gmtime_r(&now, &tm) || strftime(dateutc, sizeof dateutc, "%Y-%m-%d %H:%M:%S", &tm) == 0)
    snprintf(dateutc, sizeof dateutc, "1970-01-01 00:00:00");

  char passkey[33], jpegMd5[33];
  if (!md5Hex((const uint8_t *)s_macUp, strlen(s_macUp), passkey) || !md5Hex(jpg, jpgN, jpegMd5)) {
    snprintf(err, errn, "Ecowitt upload failed");
    return false;
  }
  s_reupload++;
  KERRY_LOGI("ecowitt", "upload mac=%s bytes=%u interval=%d reupload=%u", s_macLo, (unsigned)jpgN, intervalMin,
             (unsigned)s_reupload);

  size_t cap = jpgN + 2048;
  uint8_t *pkt = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pkt) pkt = (uint8_t *)malloc(cap);
  if (!pkt) {
    snprintf(err, errn, "Ecowitt upload failed, out of memory");
    return false;
  }
  size_t used = 0;
  char tmp[16];
  bool bad = false;
  bad |= !appendPart(pkt, cap, &used, "mac", s_macLo);
  bad |= !appendPart(pkt, cap, &used, "PASSKEY", passkey);
  bad |= !appendPart(pkt, cap, &used, "stationtype", STATION_TYPE);
  bad |= !appendPart(pkt, cap, &used, "model", MODEL);
  bad |= !appendPart(pkt, cap, &used, "dateutc", dateutc);
  bad |= !appendPart(pkt, cap, &used, "battery", "0");
  snprintf(tmp, sizeof tmp, "%u", (unsigned)s_reupload);
  bad |= !appendPart(pkt, cap, &used, "reupload", tmp);
  snprintf(tmp, sizeof tmp, "%d", intervalMin);
  bad |= !appendPart(pkt, cap, &used, "interval", tmp);
  bad |= !appendPart(pkt, cap, &used, "md5", jpegMd5);
  char hdr[192];
  int hdrN = snprintf(hdr, sizeof hdr,
                      "--%s\r\nContent-Disposition: form-data; name=\"weather_image\"; filename=\"%lu.jpg\"\r\n"
                      "Content-Type: image/jpeg\r\n\r\n",
                      BOUNDARY, (unsigned long)now);
  if (hdrN < 0 || hdrN >= (int)sizeof hdr) bad = true;
  if (!bad) bad = !appendStr(pkt, cap, &used, hdr);
  if (!bad) bad = !appendMem(pkt, cap, &used, jpg, jpgN);
  if (!bad) bad = !appendStr(pkt, cap, &used, "\r\n------DataPackageBoundary--\r\n");
  if (bad) {
    free(pkt);
    snprintf(err, errn, "Ecowitt upload failed, out of memory");
    return false;
  }

  static const char *hosts[] = {
      "http://rtpmedia.ecowitt.net/data/upload_image",
      "http://cdnrtpmedia.ecowitt.net/data/upload_image",
  };
  int host = s_host & 1;
  bool ok = postOne(hosts[host], pkt, used, err, errn);
  if (!ok) {
    host ^= 1;
    s_host = (uint8_t)host;
    KERRY_LOGW("ecowitt", "retry other host");
    ok = postOne(hosts[host], pkt, used, err, errn);
  }
  if (!ok && staUp()) {
    KERRY_LOGW("ecowitt", "retry after pause");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ok = postOne(hosts[host], pkt, used, err, errn);
  }
  free(pkt);
  return ok;
}

enum ShotResult { SHOT_NO_CAMERA, SHOT_HTTP_OK, SHOT_HTTP_FAIL };

static ShotResult takeShot(int intervalMin) {
  kerryPmSetActivity(KERRY_ACT_ECOWITT, true);
  kerryStatsNote(KERRY_SE_ECO_ATTEMPT, nullptr, 0);
  if (lockWait(portMAX_DELAY)) {
    s_working = true;
    unlock();
  }
  kerryWdtLenient();
  char err[96];
  uint8_t *jpg = nullptr;
  size_t jpgN = 0;
  if (!kerryCaptureUploadJpeg("ecowitt", &jpg, &jpgN, err, sizeof err)) {
    kerryWdtRestore();
    if (lockWait(portMAX_DELAY)) {
      setText(s_uploadStatus, sizeof s_uploadStatus, err);
      s_working = false;
      unlock();
    }
    KERRY_LOGW("ecowitt", "%s", err);
    kerryStatsNote(KERRY_SE_ECO_FAIL, err, 0);
    kerryPmSetActivity(KERRY_ACT_ECOWITT, false);
    return SHOT_NO_CAMERA;
  }
  bool ok = postJpeg(jpg, jpgN, intervalMin, err, sizeof err);
  free(jpg);
  kerryWdtRestore();
  if (lockWait(portMAX_DELAY)) {
    setText(s_uploadStatus, sizeof s_uploadStatus, err);
    s_working = false;
    unlock();
  }
  if (ok) kerryStatsNote(KERRY_SE_ECO_OK, nullptr, 0);
  else kerryStatsNote(KERRY_SE_ECO_FAIL, err, 0);
  kerryPmSetActivity(KERRY_ACT_ECOWITT, false);
  return ok ? SHOT_HTTP_OK : SHOT_HTTP_FAIL;
}

static void noteSchedule(const char *text) {
  if (strcmp(s_schedule, text) == 0) return;
  setText(s_schedule, sizeof s_schedule, text);
  KERRY_LOGI("ecowitt", "%s", s_schedule);
}

static void armInterval(uint8_t iv, ShotResult result, bool online) {
  if (s_en && s_seen && s_devid[0] && s_checked && iv >= 1 && iv <= 5 && online && result != SHOT_NO_CAMERA) {
    s_phase = PH_WAIT;
    s_armedSlot = iv;
    s_deadline = millis() + (uint32_t)iv * 5UL * 60000UL;
    char buf[48];
    snprintf(buf, sizeof buf, "Every %u min", (unsigned)iv * 5);
    noteSchedule(buf);
    return;
  }
  if (s_en && s_seen && s_devid[0] && s_checked && iv >= 1 && iv <= 5 && result == SHOT_NO_CAMERA) {
    s_phase = PH_RETRY;
    s_armedSlot = iv;
    s_deadline = millis() + 10000;
    noteSchedule("Camera not ready, retrying");
    return;
  }
  if (s_en && s_seen && s_devid[0] && s_checked && iv >= 1 && iv <= 5 && !online) {
    s_phase = PH_ARM;
    s_armedSlot = iv;
    s_deadline = millis() + 5000;
    noteSchedule("First picture in 5 s");
  }
}

static void handleManual(bool en, bool reg, uint8_t iv) {
  const char *why = nullptr;
  if (!en || !reg || !netUp() || kerryCaptureBlocked(&why) || !kerryCameraReady()) {
    if (lockWait(portMAX_DELAY)) {
      s_now = false;
      if (!en) setText(s_uploadStatus, sizeof s_uploadStatus, "Turn on Ecowitt upload first");
      else if (!reg) setText(s_uploadStatus, sizeof s_uploadStatus, "This camera is not registered");
      else if (!netUp()) setText(s_uploadStatus, sizeof s_uploadStatus, "Internet is not open yet");
      else if (why) setText(s_uploadStatus, sizeof s_uploadStatus, why);
      else setText(s_uploadStatus, sizeof s_uploadStatus, "Camera is not ready");
      unlock();
    }
    return;
  }
  if (!kerryTryBeginUpload()) return;
  ShotResult result = takeShot((int)iv * 5);
  kerryEndUpload();
  if (!lockWait(portMAX_DELAY)) return;
  s_now = false;
  armInterval(iv, result, netUp());
  unlock();
}

static void scheduler() {
  bool en = false, reg = false, now = false;
  uint8_t iv = 0, slot = 0xff, phase = PH_IDLE;
  uint32_t deadline = 0;
  if (!lockWait(portMAX_DELAY)) return;
  en = s_en;
  iv = s_iv;
  reg = s_checked && s_seen && s_devid[0];
  now = s_now;
  slot = s_armedSlot;
  phase = s_phase;
  deadline = s_deadline;
  unlock();

  if (now) {
    handleManual(en, reg, iv);
    return;
  }
  bool armed = en && reg && iv >= 1 && iv <= 5;
  if (!armed) {
    if (!lockWait(portMAX_DELAY)) return;
    s_phase = PH_IDLE;
    s_armedSlot = 0xff;
    if (!en) noteSchedule("Ecowitt upload is off");
    else if (!reg) noteSchedule("Waiting for registration");
    else noteSchedule("Periodic upload is off");
    unlock();
    return;
  }
  if (slot != iv || phase == PH_IDLE) {
    if (!lockWait(portMAX_DELAY)) return;
    s_armedSlot = iv;
    s_phase = PH_ARM;
    s_deadline = millis() + 5000;
    noteSchedule("First picture in 5 s");
    unlock();
    return;
  }
  if ((int32_t)(millis() - deadline) < 0) return;
  if (phase == PH_RETRY) {
    if (!lockWait(portMAX_DELAY)) return;
    s_phase = PH_ARM;
    s_deadline = millis() + 5000;
    noteSchedule("First picture in 5 s");
    unlock();
    return;
  }

  const char *why = nullptr;
  if (!netUp() || !kerryCameraReady() || kerryCaptureBlocked(&why)) {
    if (!lockWait(portMAX_DELAY)) return;
    s_phase = PH_RETRY;
    s_deadline = millis() + 10000;
    if (!netUp()) noteSchedule("Waiting for internet");
    else if (why) noteSchedule(why);
    else noteSchedule("Camera not ready, retrying");
    unlock();
    return;
  }
  if (!kerryTryBeginUpload()) {
    if (!lockWait(portMAX_DELAY)) return;
    s_phase = PH_RETRY;
    s_deadline = millis() + 10000;
    noteSchedule("Camera is busy, retrying");
    unlock();
    return;
  }
  ShotResult result = takeShot((int)iv * 5);
  kerryEndUpload();
  if (!lockWait(portMAX_DELAY)) return;
  armInterval(iv, result, netUp());
  unlock();
}

static void sessionWatch() {
  bool creds = false;
  bool confirmed = false;
  int nck = 0;
  if (!lockWait(portMAX_DELAY)) return;
  creds = s_acct[0] && s_pwd[0];
  confirmed = s_seen && s_devid[0];
  nck = s_nck;
  unlock();
  if (!creds) return;
  if ((int32_t)(millis() - s_sessionDue) < 0) return;
  if (netReadyMsg()) return;

  sessionAgain(false);
  if (lockWait(portMAX_DELAY)) {
    s_working = true;
    unlock();
  }
  char err[128];
  err[0] = 0;
  KERRY_LOGI("ecowitt", "session check cookies=%d registered=%d", nck, confirmed ? 1 : 0);
  beginAccountNet();
  bool session = ensureSession(err, sizeof err);
  bool listed = session && fetchDevices(err, sizeof err);
  if (session && !listed && strncmp(err, "Ecowitt unreachable", 19) != 0 && strncmp(err, "Ecowitt HTTP", 12) != 0 &&
      strncmp(err, "Out of memory", 13) != 0) {
    KERRY_LOGW("ecowitt", "session rejected, logging in again");
    ckClear();
    session = ensureSession(err, sizeof err);
    listed = session && fetchDevices(err, sizeof err);
  }
  endAccountNet();
  if (!lockWait(portMAX_DELAY)) return;
  if (session && listed) {
    s_checkErr[0] = 0;
    s_listErr[0] = 0;
    saveCredsLocked();
    sessionAgain(true);
    KERRY_LOGI("ecowitt", "session ok %s", s_seen ? "registered" : "not registered");
  } else if (s_seen && s_devid[0]) {
    setText(s_listErr, sizeof s_listErr, err[0] ? err : "Could not refresh the Ecowitt session");
    s_sessionDue = millis() + 15UL * 60UL * 1000UL;
    KERRY_LOGW("ecowitt", "session refresh failed, uploads continue: %s", err);
  } else {
    setText(s_checkErr, sizeof s_checkErr, err[0] ? err : "Could not check registration");
    KERRY_LOGW("ecowitt", "session retry in 3 min: %s", err);
  }
  s_working = false;
  s_checked = true;
  unlock();
}

static void ecowittTask(void *arg) {
  (void)arg;
  for (;;) {
    int job = JOB_NONE;
    char acct[81], pwd[65], name[32];
    acct[0] = pwd[0] = name[0] = 0;
    if (lockWait(portMAX_DELAY)) {
      job = s_job;
      if (job) {
        setText(acct, sizeof acct, s_jobAcct);
        setText(pwd, sizeof pwd, s_jobPwd);
        setText(name, sizeof name, s_jobName);
        memset(s_jobPwd, 0, sizeof s_jobPwd);
      }
      unlock();
    }
    if (job == JOB_LOGIN) runLogin(acct, pwd);
    else if (job == JOB_REGISTER) runRegister(name);
    else if (job == JOB_LOGOUT) runLogout();
    else if (job) finishMsg("Bad request", "");
    else {
      sessionWatch();
      scheduler();
    }
    if (job && lockWait(portMAX_DELAY)) {
      s_job = JOB_NONE;
      unlock();
    }
    memset(pwd, 0, sizeof pwd);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void composeSummary(char *dst, size_t n) {
  if (!s_en) setText(dst, n, "Off");
  else if (!s_checked) setText(dst, n, "Checking account");
  else if (!(s_seen && s_devid[0])) setText(dst, n, "Not registered");
  else if (s_uploadStatus[0]) setText(dst, n, s_uploadStatus);
  else if (s_schedule[0]) setText(dst, n, s_schedule);
  else setText(dst, n, "On");
}

static bool queueJob(int job) {
  if (!s_task) return false;
  if (!lockWait(pdMS_TO_TICKS(2000))) return false;
  if (s_job || s_working) {
    unlock();
    return false;
  }
  s_job = job;
  unlock();
  return true;
}

void ecowittInit() {
  loadMac();
  loadCreds();
  s_seen = false;
  s_checked = !(s_acct[0] && s_pwd[0]);
  s_mu = xSemaphoreCreateMutex();
  if (!selfCheck()) KERRY_LOGE("ecowitt", "protocol self-check failed");
  else KERRY_LOGI("ecowitt", "protocol self-check ok");
  s_httpBody = (char *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_httpBody) s_httpBody = (char *)malloc(16384);
  s_scratch = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_scratch) s_scratch = (char *)malloc(2048);
  if (!s_mu || !s_httpBody || !s_scratch) {
    KERRY_LOGE("ecowitt", "init failed");
    return;
  }
  if (xTaskCreate(ecowittTask, "ecowitt", 12288, nullptr, 1, &s_task) != pdPASS) {
    s_task = nullptr;
    KERRY_LOGE("ecowitt", "task failed");
    return;
  }
  KERRY_LOGI("ecowitt", "ready mac=%s account=%s", s_macUp, s_acct[0] ? "saved" : "none");
}

void ecowittFillStatus(JsonDocument &doc) {
  double lat = 0, lon = 0;
  char iana[40];
  bool loc = kerryTimeGetLocation(&lat, &lon, iana, sizeof iana);
  if (!s_mu || !lockWait(pdMS_TO_TICKS(2000))) {
    doc["ok"] = false;
    doc["message"] = "Ecowitt is busy";
    return;
  }
  doc["ok"] = true;
  doc["busy"] = (s_job != JOB_NONE || s_working || s_now) ? 1 : 0;
  doc["enabled"] = s_en ? 1 : 0;
  doc["interval"] = s_iv;
  doc["account"] = s_acct;
  doc["pwdSet"] = s_pwd[0] ? 1 : 0;
  doc["uid"] = s_uid;
  doc["nickname"] = s_nick;
  doc["loggedIn"] = (s_nck > 0 && s_uid[0]) ? 1 : 0;
  bool reg = s_checked && s_seen && s_devid[0];
  doc["registered"] = reg ? 1 : 0;
  doc["deviceId"] = reg ? s_devid : "";
  doc["checked"] = s_checked ? 1 : 0;
  doc["checkError"] = s_checkErr;
  doc["listError"] = s_listErr;
  doc["msg"] = s_msg;
  doc["mac"] = s_macUp;
  doc["suggestName"] = SUGGEST_NAME;
  doc["uploadStatus"] = s_uploadStatus;
  doc["schedule"] = s_schedule;
  doc["hasLocation"] = loc ? 1 : 0;
  if (loc) {
    char coord[24];
    snprintf(coord, sizeof coord, "%.6f", lat);
    doc["lat"] = coord;
    snprintf(coord, sizeof coord, "%.6f", lon);
    doc["lon"] = coord;
    doc["tz"] = iana;
  }
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (int i = 0; i < s_ndev; i++) {
    JsonObject one = arr.add<JsonObject>();
    one["id"] = s_devs[i].id;
    one["name"] = s_devs[i].name;
    one["mac"] = s_devs[i].mac;
    one["model"] = s_devs[i].model;
    one["zone"] = s_devs[i].zone;
    one["online"] = s_devs[i].online;
    one["mine"] = s_devs[i].mine ? 1 : 0;
  }
  unlock();
}

const char *ecowittSummary() {
  static char buf[96];
  if (!s_mu || !lockWait(pdMS_TO_TICKS(200))) {
    setText(buf, sizeof buf, "Busy");
    return buf;
  }
  composeSummary(buf, sizeof buf);
  unlock();
  return buf;
}

const char *ecowittSetSchedule(bool enabled, int intervalCode) {
  if (intervalCode < 0 || intervalCode > 5) return "Interval must be 0, 5, 10, 15, 20, or 25 minutes";
  if (!s_mu || !lockWait(pdMS_TO_TICKS(2000))) return "Ecowitt is busy";
  s_en = enabled;
  s_iv = (uint8_t)intervalCode;
  s_armedSlot = 0xff;
  saveSchedLocked();
  unlock();
  KERRY_LOGI("ecowitt", "schedule %s, slot %d", enabled ? "on" : "off", intervalCode);
  return nullptr;
}

const char *ecowittQueueLogin(const char *account, const char *password) {
  if (!s_task || !s_httpBody) return "Ecowitt did not start";
  if (account && strlen(account) > 80) return "Account or password is too long";
  if (password && strlen(password) > 64) return "Account or password is too long";
  const char *net = netReadyMsg();
  if (net) return net;
  if (!lockWait(pdMS_TO_TICKS(2000))) return "Ecowitt is busy";
  if (s_job || s_working) {
    unlock();
    return "Ecowitt is busy";
  }
  setText(s_jobAcct, sizeof s_jobAcct, account ? account : "");
  trim(s_jobAcct);
  setText(s_jobPwd, sizeof s_jobPwd, password ? password : "");
  s_jobName[0] = 0;
  s_job = JOB_LOGIN;
  unlock();
  return nullptr;
}

const char *ecowittQueueRegister(const char *name) {
  if (!s_task || !s_httpBody) return "Ecowitt did not start";
  char trimmed[32];
  setText(trimmed, sizeof trimmed, name ? name : "");
  trim(trimmed);
  if (!nameOk(trimmed)) return "Device name: letters, numbers, space, . _ -";
  const char *net = netReadyMsg();
  if (net) return net;
  double lat = 0, lon = 0;
  char iana[40];
  if (!kerryTimeGetLocation(&lat, &lon, iana, sizeof iana)) return "No GeoPulse position or time zone yet";
  if (!lockWait(pdMS_TO_TICKS(2000))) return "Ecowitt is busy";
  if (s_job || s_working) {
    unlock();
    return "Ecowitt is busy";
  }
  setText(s_jobName, sizeof s_jobName, trimmed);
  s_jobAcct[0] = 0;
  s_jobPwd[0] = 0;
  s_job = JOB_REGISTER;
  unlock();
  return nullptr;
}

const char *ecowittQueueLogout() {
  if (!s_task) return "Ecowitt did not start";
  if (!queueJob(JOB_LOGOUT)) return "Ecowitt is busy";
  return nullptr;
}

const char *ecowittQueueUpload() {
  if (!s_task) return "Ecowitt did not start";
  const char *why = nullptr;
  if (kerryCaptureBlocked(&why)) return why;
  if (!netUp()) return "Internet is not open yet";
  if (!kerryCameraReady()) return "Camera is not ready";
  if (!lockWait(pdMS_TO_TICKS(2000))) return "Ecowitt is busy";
  if (s_job || s_working || s_now) {
    unlock();
    return "Ecowitt is busy";
  }
  if (!s_en) {
    unlock();
    return "Turn on Ecowitt upload first";
  }
  if (!s_checked) {
    unlock();
    return "Registration has not been confirmed yet";
  }
  if (!(s_seen && s_devid[0])) {
    unlock();
    return "This camera is not registered";
  }
  s_now = true;
  unlock();
  return nullptr;
}
