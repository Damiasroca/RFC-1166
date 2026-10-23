#include "kerry_postmortem.h"

#include "kerry_diag_fs.h"
#include "kerry_log.h"
#include "kerry_version.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <nvs.h>

#include <esp_partition.h>
#include <esp_system.h>

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include <esp_core_dump.h>
#endif

#include <string.h>
#include <time.h>

#define KPM_NS "kpm"
#define REPORT_MAX KERRY_DIAG_REPORT_MAX

static uint32_t s_bootSeq = 0;
static char s_curReport[REPORT_MAX];
static bool s_haveCurReport = false;
static bool s_eraseCoredump = false;

static const char *resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
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
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_PWR_GLITCH:
      return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default:
      return "UNKNOWN";
  }
}

static const char *classify(esp_reset_reason_t r, bool deliberate) {
  if (deliberate) return "deliberate";
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
      r == ESP_RST_CPU_LOCKUP)
    return "crash";
  if (r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT) return "power";
  return "other";
}

static void activityNames(uint32_t bits, JsonArray arr) {
  struct {
    uint32_t bit;
    const char *name;
  } map[] = {{KERRY_ACT_UPLOAD, "upload"},
             {KERRY_ACT_CAPTURE, "capture"},
             {KERRY_ACT_STREAM, "stream"},
             {KERRY_ACT_LIVEPUSH, "livepush"},
             {KERRY_ACT_OTA, "ota"},
             {KERRY_ACT_SETTLE, "settle"},
             {KERRY_ACT_ECOWITT, "ecowitt"},
             {KERRY_ACT_CAM_INIT, "cam_init"}};
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
    if (bits & map[i].bit) arr.add(map[i].name);
  }
}

static void appendPrevLines(JsonArray lines, const kerry_pm_rtc_t *pm) {
  if (!pm || !pm->lineCount) return;
  uint16_t n = pm->lineCount;
  uint16_t start = (uint16_t)((pm->lineHead + KERRY_PM_LINES - n) % KERRY_PM_LINES);
  for (uint16_t i = 0; i < n; i++) {
    uint16_t idx = (uint16_t)((start + i) % KERRY_PM_LINES);
    if (pm->lines[idx][0]) lines.add(pm->lines[idx]);
  }
}

static void appendCoredump(JsonObject obj, const kerry_pm_rtc_t *prev) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  const esp_partition_t *part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!part) {
    obj["coredump"] = "no coredump partition";
    return;
  }
  if (esp_core_dump_image_check() != ESP_OK) {
    obj["coredump"] = "none";
    return;
  }
  esp_core_dump_summary_t sum;
  memset(&sum, 0, sizeof sum);
  if (esp_core_dump_get_summary(&sum) != ESP_OK) {
    obj["coredump"] = "read failed";
    return;
  }
  JsonObject cd = obj["coredump"].to<JsonObject>();
  cd["task"] = sum.exc_task;
  cd["pc"] = sum.exc_pc;
  cd["excCause"] = sum.ex_info.exc_cause;
  cd["excVaddr"] = sum.ex_info.exc_vaddr;
  cd["corrupted"] = sum.exc_bt_info.corrupted;
  JsonArray bt = cd["backtrace"].to<JsonArray>();
  for (int i = 0; i < sum.exc_bt_info.depth && i < 32; i++) bt.add(sum.exc_bt_info.bt[i]);
  char sha[17];
  memcpy(sha, sum.app_elf_sha256, 16);
  sha[16] = '\0';
  cd["elfSha"] = sha;
#if defined(ARDUINO_XIAO_ESP32S3)
  const char *tool = "xtensa-esp32s3-elf-addr2line";
#else
  const char *tool = "xtensa-esp32-elf-addr2line";
#endif
  char hint[512];
  int off = snprintf(hint, sizeof hint, "%s -pfiaC -e firmware.elf 0x%lx", tool, (unsigned long)sum.exc_pc);
  for (int i = 0; i < sum.exc_bt_info.depth && i < 32 && off > 0 && off < (int)sizeof hint - 16; i++) {
    off += snprintf(hint + off, sizeof hint - (size_t)off, " 0x%lx", (unsigned long)sum.exc_bt_info.bt[i]);
  }
  cd["decodeHint"] = hint;
  if (prev && prev->fwGit[0]) cd["fwGit"] = prev->fwGit;
  else cd["fwGit"] = "unknown";
  s_eraseCoredump = true;
#else
  (void)prev;
  obj["coredump"] = "not enabled in this build";
#endif
}

static void truncateLines(JsonDocument &doc) {
  while (doc.overflowed() || measureJson(doc) > REPORT_MAX) {
    JsonArray lines = doc["prevLines"].as<JsonArray>();
    if (lines.isNull() || lines.size() == 0) break;
    lines.remove(0);
  }
}

static bool nvsCountersOk() {
  nvs_stats_t st;
  if (nvs_get_stats(nullptr, &st) != ESP_OK) return true;
  return st.free_entries >= 40;
}

static void bumpCounter(Preferences &prefs, const char *key) {
  uint32_t v = prefs.getUInt(key, 0);
  prefs.putUInt(key, v + 1);
}

void kerryPmReportsInit() {
  s_eraseCoredump = false;
  esp_reset_reason_t rr = esp_reset_reason();
  const kerry_pm_rtc_t *prev = kerryPmPrevious();
  bool deliberate = (rr == ESP_RST_SW) && prev && prev->cause[0];

  if (nvsCountersOk()) {
    Preferences prefs;
    if (prefs.begin(KPM_NS, false)) {
      s_bootSeq = prefs.getUInt("bootSeq", 0) + 1;
      prefs.putUInt("bootSeq", s_bootSeq);
      bumpCounter(prefs, "n_total");
      char rkey[16];
      snprintf(rkey, sizeof rkey, "n_%s", resetName(rr));
      if (strlen(rkey) > 15) rkey[15] = '\0';
      bumpCounter(prefs, rkey);
      const char *cls = classify(rr, deliberate);
      if (strcmp(cls, "deliberate") == 0) bumpCounter(prefs, "n_deliberate");
      else if (strcmp(cls, "crash") == 0) bumpCounter(prefs, "n_crash");
      else if (strcmp(cls, "power") == 0) bumpCounter(prefs, "n_power");
      prefs.end();
    }
  } else {
    if (kerry_log_every_ms("kpm_nvs_low", 60000)) KERRY_LOGW("kpm", "NVS low, skipping reset counter update");
    uint32_t nvsSeq = 0;
    Preferences prefs;
    if (prefs.begin(KPM_NS, true)) {
      nvsSeq = prefs.getUInt("bootSeq", 0);
      prefs.end();
    }
    uint32_t fsSeq = kerryDiagMaxReportSeq();
    s_bootSeq = nvsSeq > fsSeq ? nvsSeq : fsSeq;
    s_bootSeq++;
  }

  JsonDocument doc;
  doc["bootSeq"] = s_bootSeq;
  doc["reason"] = resetName(rr);
  doc["class"] = classify(rr, deliberate);
  doc["bootTime"] = 0;
  if (deliberate && prev) doc["cause"] = prev->cause;
  if (prev && kerryPmHadValidPrevious()) {
    doc["prevUptimeS"] = prev->uptimeS;
    doc["prevEpoch"] = prev->epoch;
    doc["heapFree"] = prev->heapFree;
    doc["heapMin"] = prev->heapMin;
    doc["heapLargest"] = prev->heapLargest;
    doc["internalFree"] = prev->internalFree;
    doc["internalMin"] = prev->internalMin;
    doc["psramFree"] = prev->psramFree;
    doc["psramLargest"] = prev->psramLargest;
    doc["rssi"] = prev->rssi;
    doc["wifiStatus"] = prev->wifiStatus;
    activityNames(prev->activity, doc["activity"].to<JsonArray>());
    doc["fwGit"] = prev->fwGit;
    doc["fwVersion"] = prev->fwVersion;
    appendPrevLines(doc["prevLines"].to<JsonArray>(), prev);
  }
  appendCoredump(doc.as<JsonObject>(), prev);
  truncateLines(doc);
  s_haveCurReport = false;
  size_t n = serializeJson(doc, s_curReport, sizeof s_curReport);
  if (n > 0) {
    s_haveCurReport = true;
    bool stored = kerryDiagStoreReport(s_bootSeq, s_curReport, n);
    kerryDiagClaimLastLog(s_bootSeq);
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (s_eraseCoredump && stored) esp_core_dump_image_erase();
#endif
  }
}

void kerryPmReportsOnClockSync() {
  if (!s_haveCurReport) return;
  time_t now = 0;
  time(&now);
  if (now < 1600000000) return;
  JsonDocument doc;
  if (deserializeJson(doc, s_curReport)) return;
  doc["bootTime"] = (uint32_t)now;
  size_t n = serializeJson(doc, s_curReport, sizeof s_curReport);
  if (n == 0) return;
  kerryDiagUpdateReport(s_bootSeq, s_curReport, n);
}

void kerryPmFillReportsJson(JsonDocument &doc) {
  doc["bootSeq"] = s_bootSeq;
  doc["reason"] = resetName(esp_reset_reason());
  doc["storage"] = kerryDiagStorageLabel();
  bool deliberate = false;
  const kerry_pm_rtc_t *prev = kerryPmPrevious();
  if (esp_reset_reason() == ESP_RST_SW && prev && prev->cause[0]) deliberate = true;
  doc["class"] = classify(esp_reset_reason(), deliberate);
  doc["uptimeS"] = (uint32_t)(millis() / 1000UL);
  JsonObject ctr = doc["counters"].to<JsonObject>();
  Preferences prefs;
  if (prefs.begin(KPM_NS, true)) {
    ctr["total"] = prefs.getUInt("n_total", 0);
    ctr["deliberate"] = prefs.getUInt("n_deliberate", 0);
    ctr["crash"] = prefs.getUInt("n_crash", 0);
    ctr["power"] = prefs.getUInt("n_power", 0);
    JsonObject byReason = ctr["byReason"].to<JsonObject>();
    esp_reset_reason_t reasons[] = {ESP_RST_POWERON, ESP_RST_EXT,          ESP_RST_SW,
                                    ESP_RST_PANIC,   ESP_RST_INT_WDT,      ESP_RST_TASK_WDT,
                                    ESP_RST_WDT,     ESP_RST_DEEPSLEEP,    ESP_RST_BROWNOUT,
                                    ESP_RST_SDIO,    ESP_RST_USB,          ESP_RST_JTAG,
                                    ESP_RST_EFUSE,   ESP_RST_PWR_GLITCH,   ESP_RST_CPU_LOCKUP,
                                    ESP_RST_UNKNOWN};
    for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
      char rkey[16];
      snprintf(rkey, sizeof rkey, "n_%s", resetName(reasons[i]));
      if (strlen(rkey) > 15) rkey[15] = '\0';
      uint32_t v = prefs.getUInt(rkey, 0);
      if (v) byReason[resetName(reasons[i])] = v;
    }
    prefs.end();
  }
  kerryDiagListReports(doc["reports"].to<JsonArray>());
}

void kerryPmClearReportsAndCounters() {
  Preferences prefs;
  if (prefs.begin(KPM_NS, false)) {
    prefs.clear();
    prefs.end();
  }
  kerryDiagClearReports();
  s_bootSeq = 0;
  s_haveCurReport = false;
  s_curReport[0] = '\0';
}
