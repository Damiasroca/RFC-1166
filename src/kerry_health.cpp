#include "kerry_health.h"

#include "kerry_log.h"
#include "kerry_postmortem.h"
#include "kerry_stats.h"

#include <Preferences.h>
#include <cstring>
#include <esp_attr.h>
#include <esp_system.h>
#include <time.h>

#define RTC_MAGIC 0x4B455257u

typedef struct {
  uint32_t magic;
  char cause[64];
  uint32_t stamp[8];
  uint8_t n;
} rtc_wd_t;

static RTC_DATA_ATTR rtc_wd_t s_rtc;
static esp_reset_reason_t s_reset;
static uint8_t s_camFail = 0;
static uint8_t s_upFail = 0;
static bool s_rebooting = false;
static bool s_held = false;

bool g_wd_en = true;
uint8_t g_wd_cam_n = 3;
uint8_t g_wd_up_n = 3;
uint8_t g_wd_cap_n = 3;
uint16_t g_wd_cap_m = 60;

static void rtcClear() { memset(&s_rtc, 0, sizeof s_rtc); }

static uint32_t nowSec() {
  time_t t = 0;
  time(&t);
  return t > 0 ? (uint32_t)t : (uint32_t)(millis() / 1000UL);
}

static uint8_t capFresh(uint32_t now) {
  uint32_t win = (uint32_t)g_wd_cap_m * 60u;
  if (win == 0) win = 60;
  uint8_t n = 0;
  if (s_rtc.magic != RTC_MAGIC) return 0;
  for (uint8_t i = 0; i < s_rtc.n && i < 8; i++) {
    uint32_t s = s_rtc.stamp[i];
    if (now >= s && (now - s) < win) n++;
  }
  return n;
}

static bool capAllows() {
  uint8_t cap = g_wd_cap_n ? g_wd_cap_n : 3;
  return capFresh(nowSec()) < cap;
}

static void capRecord() {
  uint32_t now = nowSec();
  uint32_t win = (uint32_t)g_wd_cap_m * 60u;
  if (win == 0) win = 60;
  uint32_t keep[8];
  uint8_t kn = 0;
  if (s_rtc.magic == RTC_MAGIC) {
    for (uint8_t i = 0; i < s_rtc.n && i < 8; i++) {
      uint32_t s = s_rtc.stamp[i];
      if (now >= s && (now - s) < win && kn < 8) keep[kn++] = s;
    }
  }
  if (kn >= 8) {
    memmove(keep, keep + 1, 7 * sizeof keep[0]);
    kn = 7;
  }
  keep[kn++] = now;
  memset(&s_rtc.stamp, 0, sizeof s_rtc.stamp);
  memcpy(s_rtc.stamp, keep, kn * sizeof keep[0]);
  s_rtc.n = kn;
  s_rtc.magic = RTC_MAGIC;
}

static char s_rebootCause[80];

static void rebootSoon() {
  if (s_rebooting) return;
  s_rebooting = true;
  delay(1500);
  kerryRebootWithCause(s_rebootCause[0] ? s_rebootCause : "watchdog");
}

static bool maybeReboot(const char *detail) {
  if (!g_wd_en || s_rebooting) return false;
  if (!capAllows()) {
    s_held = true;
    kerryStatsNote(KERRY_SE_WD_HELD, detail, 0);
    KERRY_LOGW("health", "watchdog reboot held (%s, cap %u/%u in %u min)", detail ? detail : "watchdog",
               (unsigned)capFresh(nowSec()), (unsigned)(g_wd_cap_n ? g_wd_cap_n : 3), (unsigned)g_wd_cap_m);
    return false;
  }
  s_held = false;
  capRecord();
  strncpy(s_rtc.cause, detail ? detail : "watchdog", sizeof s_rtc.cause - 1);
  s_rtc.cause[sizeof s_rtc.cause - 1] = '\0';
  s_rtc.magic = RTC_MAGIC;
  snprintf(s_rebootCause, sizeof s_rebootCause, "watchdog: %s", detail ? detail : "watchdog");
  kerryStatsNote(KERRY_SE_WD_REBOOT, detail, 0);
  KERRY_LOGW("health", "watchdog reboot: %s", s_rtc.cause);
  rebootSoon();
  return true;
}

static const char *resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    default:
      return "OTHER";
  }
}

static void loadWd() {
  Preferences prefs;
  prefs.begin("kerry", true);
  g_wd_en = prefs.getBool("wd_en", true);
  g_wd_cam_n = prefs.getUChar("wd_cam", 3);
  g_wd_up_n = prefs.getUChar("wd_up", 3);
  g_wd_cap_n = prefs.getUChar("wd_cap", 3);
  g_wd_cap_m = prefs.getUShort("wd_cap_m", 60);
  prefs.end();
}

static void saveWd() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putBool("wd_en", g_wd_en);
  prefs.putUChar("wd_cam", g_wd_cam_n);
  prefs.putUChar("wd_up", g_wd_up_n);
  prefs.putUChar("wd_cap", g_wd_cap_n);
  prefs.putUShort("wd_cap_m", g_wd_cap_m);
  prefs.end();
}

void kerryHealthInit() {
  loadWd();
  s_reset = esp_reset_reason();
  if (s_reset == ESP_RST_POWERON || s_reset == ESP_RST_BROWNOUT) rtcClear();
  else if (s_rtc.magic != RTC_MAGIC) rtcClear();
  KERRY_LOGI("health", "reset=%s wd=%s cam=%u up=%u cap=%u/%umin", resetName(s_reset), g_wd_en ? "on" : "off",
             (unsigned)g_wd_cam_n, (unsigned)g_wd_up_n, (unsigned)g_wd_cap_n, (unsigned)g_wd_cap_m);
  if (s_rtc.magic == RTC_MAGIC && s_rtc.cause[0]) KERRY_LOGW("health", "last watchdog: %s", s_rtc.cause);
}

void kerryHealthFillStatus(JsonDocument &doc) {
  doc["wd_en"] = g_wd_en;
  doc["wd_cam_n"] = g_wd_cam_n;
  doc["wd_up_n"] = g_wd_up_n;
  doc["wd_cap_n"] = g_wd_cap_n;
  doc["wd_cap_m"] = g_wd_cap_m;
  doc["wd_cam_fail"] = s_camFail;
  doc["wd_up_fail"] = s_upFail;
  doc["wd_held"] = s_held;
  doc["resetReason"] = resetName(s_reset);
  if (s_rtc.magic == RTC_MAGIC && s_rtc.cause[0]) doc["lastWdCause"] = s_rtc.cause;
}

const char *kerryHealthSetWatchdog(bool enabled, int camN, int upN, int capN, int capM) {
  if (camN < 1 || camN > 20 || upN < 1 || upN > 20 || capN < 1 || capN > 20 || capM < 1 || capM > 1440)
    return "watchdog values out of range";
  g_wd_en = enabled;
  g_wd_cam_n = (uint8_t)camN;
  g_wd_up_n = (uint8_t)upN;
  g_wd_cap_n = (uint8_t)capN;
  g_wd_cap_m = (uint16_t)capM;
  saveWd();
  KERRY_LOGI("health", "watchdog config en=%d cam=%d up=%d cap=%d/%umin", enabled ? 1 : 0, camN, upN, capN, capM);
  return nullptr;
}

void kerryHealthNoteCameraOk() { s_camFail = 0; }

bool kerryHealthNoteCameraFail(const char *detail) {
  kerryStatsNote(KERRY_SE_CAM_FAIL, detail, 0);
  if (s_camFail < 255) s_camFail++;
  uint8_t n = g_wd_cam_n ? g_wd_cam_n : 3;
  KERRY_LOGW("health", "camera fail %u/%u (%s)", (unsigned)s_camFail, (unsigned)n, detail ? detail : "?");
  if (s_camFail >= n) return maybeReboot(detail ? detail : "camera");
  return false;
}

void kerryHealthNoteUploadOk() { s_upFail = 0; }

bool kerryHealthNoteUploadFail(const char *detail) {
  if (s_upFail < 255) s_upFail++;
  uint8_t n = g_wd_up_n ? g_wd_up_n : 3;
  KERRY_LOGW("health", "upload fail %u/%u (%s)", (unsigned)s_upFail, (unsigned)n, detail ? detail : "?");
  if (s_upFail >= n) return maybeReboot(detail ? detail : "upload");
  return false;
}

bool kerryHealthNoteNetFail(const char *detail) {
  KERRY_LOGW("health", "net fail (%s)", detail ? detail : "?");
  return maybeReboot(detail ? detail : "network");
}
