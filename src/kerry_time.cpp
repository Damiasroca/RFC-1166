#include "kerry_time.h"

#include "kerry_log.h"
#include "tz_lookup.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <time.h>

static bool s_sntpStarted = false;
static char s_tzIana[40];
static char s_tzPosix[64];
static double s_lat = 0;
static double s_lon = 0;
static bool s_hasCoords = false;
static portMUX_TYPE s_tzMux = portMUX_INITIALIZER_UNLOCKED;

static void applyTz() {
  if (!s_tzPosix[0]) return;
  setenv("TZ", s_tzPosix, 1);
  tzset();
}

static void persistTz() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putString("tz_iana", s_tzIana);
  prefs.putString("tz_posix", s_tzPosix);
  prefs.putBool("tz_loc", s_hasCoords);
  if (s_hasCoords) {
    prefs.putDouble("tz_lat", s_lat);
    prefs.putDouble("tz_lon", s_lon);
  }
  prefs.end();
}

static void loadTz() {
  Preferences prefs;
  prefs.begin("kerry", true);
  String iana = prefs.getString("tz_iana", "");
  String posix = prefs.getString("tz_posix", "");
  strncpy(s_tzIana, iana.c_str(), sizeof s_tzIana - 1);
  strncpy(s_tzPosix, posix.c_str(), sizeof s_tzPosix - 1);
  s_hasCoords = prefs.getBool("tz_loc", false);
  s_lat = prefs.getDouble("tz_lat", 0);
  s_lon = prefs.getDouble("tz_lon", 0);
  prefs.end();
  if (!s_tzPosix[0] && s_tzIana[0]) {
    const char *p = kerry_tz_posix(s_tzIana);
    if (p) strncpy(s_tzPosix, p, sizeof s_tzPosix - 1);
  }
  applyTz();
}

static void startSntp() {
  if (s_sntpStarted || WiFi.status() != WL_CONNECTED) return;
  configTime(0, 0, "pool.ntp.org", "time.windows.com", "time.nist.gov");
  s_sntpStarted = true;
  KERRY_LOGI("time", "SNTP started (tz=%s)", s_tzIana[0] ? s_tzIana : "unset");
}

void kerryTimeInit() {
  loadTz();
  if (s_tzIana[0]) {
    KERRY_LOGI("time", "loaded tz=%s (%s)", s_tzIana, s_tzPosix);
  } else {
    KERRY_LOGI("time", "no timezone saved yet");
  }
}

void kerryTimeOnWifiUp() { startSntp(); }

void kerryTimeLoop() {
  if (WiFi.status() == WL_CONNECTED && !s_sntpStarted) startSntp();
}

void kerryTimeUpdateFromCoords(double lat, double lon) {
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;
  portENTER_CRITICAL(&s_tzMux);
  bool unchanged = s_hasCoords && fabs(s_lat - lat) < 0.05 && fabs(s_lon - lon) < 0.05 && s_tzPosix[0];
  portEXIT_CRITICAL(&s_tzMux);
  if (unchanged) return;

  char iana[40];
  if (!kerry_tz_from_coords(lat, lon, iana, sizeof iana)) return;
  const char *posix = kerry_tz_posix(iana);
  if (!posix) return;

  portENTER_CRITICAL(&s_tzMux);
  s_lat = lat;
  s_lon = lon;
  s_hasCoords = true;
  strncpy(s_tzIana, iana, sizeof s_tzIana - 1);
  s_tzIana[sizeof s_tzIana - 1] = '\0';
  strncpy(s_tzPosix, posix, sizeof s_tzPosix - 1);
  s_tzPosix[sizeof s_tzPosix - 1] = '\0';
  portEXIT_CRITICAL(&s_tzMux);
  applyTz();
  persistTz();
  KERRY_LOGI("time", "timezone from coords %.4f,%.4f -> %s", lat, lon, s_tzIana);
}

bool kerryTimeGetLocation(double *lat, double *lon, char *iana, size_t ianaLen) {
  if (!iana || ianaLen == 0) return false;
  portENTER_CRITICAL(&s_tzMux);
  bool ok = s_hasCoords && s_tzIana[0];
  if (ok) {
    if (lat) *lat = s_lat;
    if (lon) *lon = s_lon;
    strncpy(iana, s_tzIana, ianaLen - 1);
    iana[ianaLen - 1] = '\0';
  }
  portEXIT_CRITICAL(&s_tzMux);
  return ok;
}

void kerryTimeFillStatus(JsonDocument &doc) {
  time_t now = 0;
  time(&now);
  doc["clockSynced"] = now >= 1600000000;
  doc["tzIana"] = s_tzIana;
  doc["tzPosix"] = s_tzPosix;
  doc["hasCoords"] = s_hasCoords;
  if (s_hasCoords) {
    doc["lat"] = s_lat;
    doc["lon"] = s_lon;
  }
  if (now >= 1600000000) {
    struct tm tm;
    char buf[32];
    if (localtime_r(&now, &tm)) {
      strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
      doc["localTime"] = buf;
      doc["dst"] = tm.tm_isdst > 0;
    }
  }
}
