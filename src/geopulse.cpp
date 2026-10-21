#include "geopulse.h"

#include "secrets.h"
#include "kerry_log.h"
#include "kerry_time.h"
#include "overlay/overlay_cfg.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include <time.h>

static const char *GP_BASE = KERRY_GEOPULSE_API_BASE;

static uint8_t json_u8(JsonVariant v, uint8_t fallback) {
  if (v.isNull()) return fallback;
  if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
  if (v.is<int>() || v.is<long>()) return v.as<int>() ? 1 : 0;
  return fallback;
}

static String gpUser;
static String gpPass;
static String gpDevice = KERRY_GEOPULSE_DEFAULT_DEVICE;
static String gpApiKey;
static time_t gpKeyExp = 0;
static ov_nav_t g_lastNav;
static uint32_t g_lastFetchMs = 0;
static const uint32_t GP_CACHE_TTL_MS = 90000;
static char g_lastError[160] = "Not fetched yet";

static void setError(const char *msg) {
  strncpy(g_lastError, msg ? msg : "Unknown error", sizeof g_lastError - 1);
  g_lastError[sizeof g_lastError - 1] = '\0';
}

static time_t parseIsoUtc(const char *iso) {
  if (!iso || !iso[0]) return 0;
  int y, mo, d, h, mi, s;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  struct tm tm = {};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  return mktime(&tm);
}

static void formatNavTime(int64_t tsMs, char *out, size_t outLen) {
  time_t sec;
  struct tm tm;
  if (!out || outLen == 0) return;
  out[0] = '\0';
  if (tsMs <= 0) return;
  if (tsMs > 1000000000000LL) sec = (time_t)(tsMs / 1000);
  else sec = (time_t)tsMs;
  if (localtime_r(&sec, &tm)) strftime(out, outLen, "%Y-%m-%d %H:%M:%S", &tm);
  else snprintf(out, outLen, "%lld", (long long)tsMs);
}

static JsonVariant pickField(JsonObject obj, const char *k1, const char *k2 = nullptr, const char *k3 = nullptr) {
  if (obj.isNull()) return JsonVariant();
  if (!obj[k1].isNull()) return obj[k1];
  if (k2 && !obj[k2].isNull()) return obj[k2];
  if (k3 && !obj[k3].isNull()) return obj[k3];
  return JsonVariant();
}

static bool variantToDouble(JsonVariant v, double *out) {
  if (!out || v.isNull()) return false;
  if (v.is<double>() || v.is<float>()) {
    *out = v.as<double>();
    return true;
  }
  if (v.is<int>() || v.is<long>() || v.is<long long>()) {
    *out = (double)v.as<long long>();
    return true;
  }
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (!s || !s[0]) return false;
    char *end = nullptr;
    double n = strtod(s, &end);
    if (end == s) return false;
    *out = n;
    return true;
  }
  return false;
}

static bool variantToStr(JsonVariant v, char *out, size_t len, int decimals = -1) {
  if (!out || len == 0) return false;
  out[0] = '\0';
  if (v.isNull()) return false;
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (!s || !s[0]) return false;
    strncpy(out, s, len - 1);
    out[len - 1] = '\0';
    return true;
  }
  if (v.is<double>() || v.is<float>()) {
    if (decimals >= 0) snprintf(out, len, "%.*f", decimals, (double)v.as<double>());
    else snprintf(out, len, "%g", (double)v.as<double>());
    return out[0] != '\0';
  }
  if (v.is<int>() || v.is<long>() || v.is<long long>()) {
    snprintf(out, len, "%lld", (long long)v.as<long long>());
    return true;
  }
  return false;
}

static void formatTimestamp(JsonVariant v, char *out, size_t outLen) {
  if (!out || outLen == 0) return;
  out[0] = '\0';
  if (v.isNull()) return;
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (!s || !s[0]) return;
    if (strchr(s, 'T') || strchr(s, '-')) {
      time_t t = parseIsoUtc(s);
      if (t > 0) {
        struct tm tm;
        if (localtime_r(&t, &tm)) {
          strftime(out, outLen, "%Y-%m-%d %H:%M:%S", &tm);
          return;
        }
      }
    }
    char *end = nullptr;
    int64_t n = strtoll(s, &end, 10);
    if (end != s && n > 0) {
      formatNavTime(n, out, outLen);
      return;
    }
    strncpy(out, s, outLen - 1);
    out[outLen - 1] = '\0';
    return;
  }
  if (v.is<double>() || v.is<float>() || v.is<int>() || v.is<long>() || v.is<long long>()) {
    formatNavTime((int64_t)v.as<long long>(), out, outLen);
  }
}

static JsonObject pickPositionRoot(JsonDocument &doc) {
  JsonObject root = doc.as<JsonObject>();
  if (root["position"].is<JsonObject>()) return root["position"].as<JsonObject>();
  if (root["data"].is<JsonObject>()) {
    JsonObject data = root["data"].as<JsonObject>();
    if (data["position"].is<JsonObject>()) return data["position"].as<JsonObject>();
    return data;
  }
  return root;
}

static JsonVariant pickMeta(JsonDocument &doc, const char *k1, const char *k2 = nullptr) {
  JsonObject root = doc.as<JsonObject>();
  JsonVariant v = pickField(root, k1, k2);
  if (!v.isNull()) return v;
  if (root["data"].is<JsonObject>()) return pickField(root["data"].as<JsonObject>(), k1, k2);
  return JsonVariant();
}

static void addAuthHeaders(HTTPClient &http) {
  http.addHeader("Authorization", String("Bearer ") + gpApiKey);
  http.addHeader("X-API-Key", gpApiKey);
}

static void setupSecureClient(WiFiClientSecure &client) { client.setInsecure(); }

static bool tlsHeapReady() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= 16384;
}

static void noteHttpError(const char *step, int code, const String &resp, String *errOut) {
  JsonDocument errDoc;
  if (!deserializeJson(errDoc, resp)) {
    const char *msg = errDoc["message"] | errDoc["error"] | "";
    if (msg[0]) {
      if (errOut) *errOut = String(step) + " HTTP " + String(code) + ": " + msg;
      return;
    }
  }
  if (errOut) *errOut = String(step) + " HTTP " + String(code);
}

static const char *pickApiKey(JsonDocument &doc) {
  JsonObject data = doc["data"].as<JsonObject>();
  JsonObject user = doc["user"].as<JsonObject>();
  const char *key = doc["apiKey"] | doc["token"] | doc["accessToken"] | doc["key"] | "";
  if (key[0]) return key;
  key = data["apiKey"] | data["token"] | data["accessToken"] | data["key"] | "";
  if (key[0]) return key;
  return user["apiKey"] | user["token"] | "";
}

void kerryGeoPulseLoadConfig() {
  Preferences prefs;
  prefs.begin("kerry", true);
  gpUser = prefs.getString("gp_user", KERRY_GEOPULSE_DEFAULT_USER);
  gpPass = prefs.getString("gp_pass", KERRY_GEOPULSE_DEFAULT_PASSWORD);
  gpDevice = prefs.getString("gp_dev", KERRY_GEOPULSE_DEFAULT_DEVICE);
  gpApiKey = prefs.getString("gp_key", "");
  gpKeyExp = (time_t)prefs.getULong64("gp_exp", 0);
  prefs.end();
  KERRY_LOGI("geopulse", "loaded user=%s device=%s key=%s", gpUser.length() ? gpUser.c_str() : "(none)",
             gpDevice.c_str(), gpApiKey.length() ? "cached" : "none");
}

void kerryGeoPulseSaveCredentials(const char *user, const char *pass, const char *device) {
  Preferences prefs;
  prefs.begin("kerry", false);
  if (user && user[0]) {
    gpUser = user;
    prefs.putString("gp_user", gpUser);
  }
  if (pass && pass[0]) {
    gpPass = pass;
    prefs.putString("gp_pass", gpPass);
  }
  if (device && device[0]) {
    gpDevice = device;
    prefs.putString("gp_dev", gpDevice);
  }
  prefs.end();
  KERRY_LOGI("geopulse", "credentials saved user=%s device=%s pass=%s", gpUser.c_str(), gpDevice.c_str(),
             pass && pass[0] ? "updated" : "unchanged");
}

static void saveKey(const char *key, time_t expUnix) {
  gpApiKey = key ? key : "";
  gpKeyExp = expUnix;
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putString("gp_key", gpApiKey);
  prefs.putULong64("gp_exp", (uint64_t)gpKeyExp);
  prefs.end();
}

static bool loginGeoPulse(String *errOut) {
  if (!gpUser.length() || !gpPass.length()) {
    if (errOut) *errOut = "GeoPulse username and password required";
    return false;
  }
  if (!tlsHeapReady()) {
    if (errOut) *errOut = "Low memory for GeoPulse login";
    return false;
  }
  JsonDocument body;
  body["username"] = gpUser;
  body["email"] = gpUser;
  body["password"] = gpPass;
  String payload;
  serializeJson(body, payload);

  int code = 0;
  String resp;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt) delay(500);
    WiFiClientSecure client;
    setupSecureClient(client);
    HTTPClient http;
    http.setTimeout(20000);
    if (!http.begin(client, String(GP_BASE) + "/login")) {
      if (attempt == 2 && errOut) *errOut = "Could not start login request";
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    code = http.POST(payload);
    resp = http.getString();
    http.end();
    if (code > 0) break;
  }
  if (code <= 0) {
    if (errOut) *errOut = "Login TLS/connect failed";
    return false;
  }
  if (code != 200) {
    noteHttpError("Login", code, resp, errOut);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    if (errOut) *errOut = "Login JSON parse failed";
    KERRY_LOGD("geopulse", "login body: %.*s", resp.length() > 300 ? 300 : (int)resp.length(), resp.c_str());
    return false;
  }
  if (doc["success"].is<bool>() && !doc["success"].as<bool>()) {
    if (errOut) *errOut = doc["message"] | doc["error"] | "Login failed";
    return false;
  }
  const char *key = pickApiKey(doc);
  if (!key[0]) {
    if (errOut) *errOut = "Login returned no API key";
    KERRY_LOGD("geopulse", "login body: %.*s", resp.length() > 300 ? 300 : (int)resp.length(), resp.c_str());
    return false;
  }
  JsonObject loginData = doc["data"].as<JsonObject>();
  time_t exp = parseIsoUtc(doc["expiresAt"] | loginData["expiresAt"] | "");
  if (exp <= time(NULL)) exp = time(NULL) + 29L * 86400L;
  saveKey(key, exp);
  KERRY_LOGD("geopulse", "login OK, key expires %ld", (long)exp);
  return true;
}

static bool ensureApiKey(String *errOut) {
  time_t now = time(NULL);
  if (gpApiKey.length() && gpKeyExp > now + 3600) return true;
  return loginGeoPulse(errOut);
}

static bool fetchPositionJson(String &outBody, int *httpCode, String *errOut) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt) delay(500);
    if (!tlsHeapReady()) {
      if (errOut) *errOut = "Low memory for GeoPulse fetch";
      return false;
    }
    if (!ensureApiKey(errOut)) return false;
    WiFiClientSecure client;
    setupSecureClient(client);
    HTTPClient http;
    http.setTimeout(20000);
    String url = String(GP_BASE) + "/api/device-position/" + gpDevice;
    if (!http.begin(client, url)) {
      if (attempt == 2 && errOut) *errOut = "Could not start position request";
      continue;
    }
    addAuthHeaders(http);
    int code = http.GET();
    outBody = http.getString();
    http.end();
    *httpCode = code;
    if (code <= 0) {
      if (attempt < 2) continue;
      if (errOut) *errOut = "Position TLS/connect failed";
      return false;
    }
    if (code == 401 && attempt < 2) {
      gpApiKey = "";
      gpKeyExp = 0;
      continue;
    }
    if (code != 200) {
      noteHttpError("Position", code, outBody, errOut);
      return false;
    }
    return true;
  }
  if (errOut) *errOut = "Position request unauthorized";
  return false;
}

static void fillNavFromJson(ov_nav_t *nav, JsonDocument &doc) {
  char lat[16];
  char lon[16];
  char speed[16];
  char speedUnit[8];
  char course[16];

  memset(nav, 0, sizeof *nav);
  JsonObject pos = pickPositionRoot(doc);

  double latVal = 0;
  double lonVal = 0;
  bool hasLat = variantToDouble(pickField(pos, "lat", "latitude"), &latVal);
  bool hasLon = variantToDouble(pickField(pos, "lon", "longitude"), &lonVal);
  variantToStr(pickField(pos, "lat", "latitude"), lat, sizeof lat, 5);
  variantToStr(pickField(pos, "lon", "longitude"), lon, sizeof lon, 5);
  if (hasLat && hasLon) kerryTimeUpdateFromCoords(latVal, lonVal);
  variantToStr(pickField(pos, "speed", "sog"), speed, sizeof speed, 1);
  if (!variantToStr(pickField(pos, "speed_unit", "speedUnit", "unit"), speedUnit, sizeof speedUnit)) {
    strncpy(speedUnit, "kts", sizeof speedUnit - 1);
  }
  variantToStr(pickField(pos, "course", "heading", "cog"), course, sizeof course, 0);

  formatTimestamp(pickField(pos, "timestamp", "time", "sampleTime"), nav->time_text, sizeof nav->time_text);

  if (lat[0]) snprintf(nav->lat, sizeof nav->lat, "Lat %s", lat);
  if (lon[0]) snprintf(nav->lon, sizeof nav->lon, "Lon %s", lon);
  if (speed[0]) snprintf(nav->speed, sizeof nav->speed, "%s %s", speed, speedUnit);
  if (course[0]) snprintf(nav->course, sizeof nav->course, "%s°", course);

  char devId[24];
  char matchType[16];
  char requested[24];
  if (variantToStr(pickMeta(doc, "deviceId", "device_id"), devId, sizeof devId))
    snprintf(nav->device_id, sizeof nav->device_id, "Dev %s", devId);
  if (variantToStr(pickMeta(doc, "matchType", "match_type"), matchType, sizeof matchType))
    snprintf(nav->match_type, sizeof nav->match_type, "Match %s", matchType);
  if (variantToStr(pickMeta(doc, "requested", "requestedId"), requested, sizeof requested))
    snprintf(nav->requested, sizeof nav->requested, "Req %s", requested);

  nav->ok = lat[0] || lon[0] || speed[0] || course[0] || nav->time_text[0];
}

bool kerryGeoPulseFetchNav(ov_nav_t *nav) {
  if (WiFi.status() != WL_CONNECTED) {
    setError("Wi-Fi not connected");
    return false;
  }
  uint32_t t0 = millis();
  String body;
  int code = 0;
  String err;
  if (!fetchPositionJson(body, &code, &err)) {
    setError(err.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    setError("Position JSON parse failed");
    KERRY_LOGD("geopulse", "position body: %.*s", body.length() > 300 ? 300 : (int)body.length(), body.c_str());
    return false;
  }
  if (doc["success"].is<bool>() && !doc["success"].as<bool>()) {
    setError(doc["message"] | doc["error"] | "GeoPulse position error");
    return false;
  }
  fillNavFromJson(&g_lastNav, doc);
  g_lastNav.fetch_ms = (int)(millis() - t0);
  if (!g_lastNav.ok) {
    setError("Position JSON had no navigation fields");
    KERRY_LOGD("geopulse", "unexpected body: %.*s", body.length() > 240 ? 240 : (int)body.length(), body.c_str());
    return false;
  }
  g_lastFetchMs = millis();
  setError("");
  KERRY_LOGD("geopulse", "nav OK %d ms lat=%s lon=%s", g_lastNav.fetch_ms, g_lastNav.lat, g_lastNav.lon);
  if (nav) *nav = g_lastNav;
  return true;
}

const ov_nav_t *kerryGeoPulseLastNav() { return &g_lastNav; }

bool kerryGeoPulseHasCredentials() { return gpUser.length() > 0 && gpPass.length() > 0; }

uint32_t kerryGeoPulseCacheAgeMs() {
  if (!g_lastFetchMs) return UINT32_MAX;
  return millis() - g_lastFetchMs;
}

bool kerryGeoPulseEnsureNav(bool forceRefresh) {
  if (!kerryGeoPulseHasCredentials()) return false;
  if (!forceRefresh && g_lastNav.ok && g_lastFetchMs && kerryGeoPulseCacheAgeMs() < GP_CACHE_TTL_MS) return true;
  return kerryGeoPulseFetchNav(nullptr);
}

const ov_nav_t *kerryGeoPulseNavForOverlay() { return &g_lastNav; }

const char *kerryGeoPulseLastError() { return g_lastError; }

void kerryGeoPulseFillConfig(JsonDocument &doc) {
  doc["enabled"] = g_ov_en;
  doc["gpUser"] = gpUser;
  doc["gpDevice"] = gpDevice;
  doc["hasPassword"] = gpPass.length() > 0;
  doc["hasApiKey"] = gpApiKey.length() > 0;
  doc["keyExpires"] = (uint64_t)gpKeyExp;
  doc["lastError"] = g_lastError;
  doc["lastFetchMs"] = g_lastNav.fetch_ms;
  doc["navOk"] = g_lastNav.ok;
  doc["cacheAgeSec"] = g_lastFetchMs ? (uint32_t)(kerryGeoPulseCacheAgeMs() / 1000UL) : 0;
  JsonArray els = doc["elements"].to<JsonArray>();
  const char *labels[] = {"Timestamp", "Latitude", "Longitude", "Speed", "Course", "Device ID", "Match type",
                          "Requested ID"};
  for (int i = 0; i < OV_EL_N; i++) {
    JsonObject row = els.add<JsonObject>();
    row["id"] = kerry_overlay_el_id(i);
    row["label"] = labels[i];
    row["en"] = g_ov_geom.en[i] != 0;
    row["an"] = g_ov_geom.an[i];
    row["x"] = g_ov_geom.ox[i];
    row["y"] = g_ov_geom.oy[i];
  }
  JsonObject sample = doc["sample"].to<JsonObject>();
  sample["time"] = g_lastNav.time_text;
  sample["lat"] = g_lastNav.lat;
  sample["lon"] = g_lastNav.lon;
  sample["speed"] = g_lastNav.speed;
  sample["course"] = g_lastNav.course;
  sample["dev"] = g_lastNav.device_id;
  sample["mtc"] = g_lastNav.match_type;
  sample["req"] = g_lastNav.requested;
}

const char *kerryGeoPulseApplyConfig(JsonDocument &in) {
  bool enabled = g_ov_en;
  if (in["enabled"].is<bool>()) enabled = in["enabled"].as<bool>();
  else if (in["enabled"].is<int>() || in["enabled"].is<long>()) enabled = in["enabled"].as<int>() != 0;
  const char *user = in["gpUser"] | "";
  const char *pass = in["gpPass"] | "";
  const char *device = in["gpDevice"] | "";
  if (user[0] || pass[0] || device[0]) kerryGeoPulseSaveCredentials(user, pass, device);
  if (device[0] && !gpDevice.length()) return "Enter a device slug";

  ov_geom_t geom = g_ov_geom;
  JsonArray els = in["elements"].as<JsonArray>();
  if (!els.isNull()) {
    for (JsonObject row : els) {
      const char *id = row["id"] | "";
      int idx = kerry_overlay_el_index(id);
      if (idx < 0) continue;
      geom.en[idx] = json_u8(row["en"], geom.en[idx]);
      geom.an[idx] = (uint8_t)json_u8(row["an"], geom.an[idx]);
      geom.ox[idx] = row["x"] | geom.ox[idx];
      geom.oy[idx] = row["y"] | geom.oy[idx];
    }
  }
  kerry_overlay_cfg_save(enabled, &geom);
  return nullptr;
}
