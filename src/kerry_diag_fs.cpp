#include "kerry_diag_fs.h"

#include "kerry_log.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_littlefs.h>
#include <nvs.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define DIAG_DIR "/littlefs/diag"
#define REPORT_MAX 2600
#define REPORT_KEEP 10
#define FAILS_PATH DIAG_DIR "/fails.bin"
#define FAILS_VER 1

static bool s_mounted = false;

static void migrateNvsReports() {
  Preferences prefs;
  if (prefs.begin("kpm", false)) {
    for (int i = 0; i <= 4; i++) {
      char key[4];
      snprintf(key, sizeof key, "r%d", i);
      prefs.remove(key);
    }
    prefs.remove("head");
    prefs.end();
  }
  if (prefs.begin("kstats", false)) {
    prefs.remove("fails");
    prefs.remove("fHead");
    prefs.remove("fCount");
    prefs.end();
  }
}

void kerryDiagFsInit() {
  if (s_mounted) return;
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/littlefs";
  conf.partition_label = "spiffs";
  conf.format_if_mount_failed = true;
  conf.grow_on_mount = true;
  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    KERRY_LOGW("diagfs", "LittleFS mount failed: %s", esp_err_to_name(err));
    return;
  }
  s_mounted = true;
  mkdir(DIAG_DIR, 0777);
  migrateNvsReports();
  nvs_stats_t st;
  if (nvs_get_stats(nullptr, &st) == ESP_OK) {
    KERRY_LOGI("diagfs", "NVS free entries=%u", (unsigned)st.free_entries);
  }
}

bool kerryDiagFsReady() { return s_mounted; }

const char *kerryDiagStorageLabel() { return s_mounted ? "littlefs" : "unavailable"; }

uint32_t kerryDiagMaxReportSeq() {
  uint32_t maxSeq = 0;
  if (!s_mounted) return 0;
  DIR *d = opendir(DIAG_DIR);
  if (!d) return 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr) {
    if (strncmp(ent->d_name, "rpt_", 4) != 0) continue;
    uint32_t seq = (uint32_t)strtoul(ent->d_name + 4, nullptr, 10);
    if (seq > maxSeq) maxSeq = seq;
  }
  closedir(d);
  return maxSeq;
}

static bool writeAtomic(const char *finalPath, const void *hdr, size_t hdrLen, const void *data, size_t len) {
  char tmp[64];
  snprintf(tmp, sizeof tmp, "%s.tmp", finalPath);
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  if (hdrLen) {
    size_t w = fwrite(hdr, 1, hdrLen, f);
    if (w != hdrLen) {
      fclose(f);
      remove(tmp);
      return false;
    }
  }
  if (len) {
    size_t w = fwrite(data, 1, len, f);
    if (w != len) {
      fclose(f);
      remove(tmp);
      return false;
    }
  }
  fclose(f);
  if (rename(tmp, finalPath) != 0) {
    remove(tmp);
    return false;
  }
  return true;
}

static void pruneReports() {
  uint32_t seqs[REPORT_KEEP + 4];
  int n = 0;
  DIR *d = opendir(DIAG_DIR);
  if (!d) return;
  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr && n < (int)(sizeof seqs / sizeof seqs[0])) {
    if (strncmp(ent->d_name, "rpt_", 4) != 0) continue;
    seqs[n++] = (uint32_t)strtoul(ent->d_name + 4, nullptr, 10);
  }
  closedir(d);
  if (n <= REPORT_KEEP) return;
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (seqs[i] > seqs[j]) {
        uint32_t t = seqs[i];
        seqs[i] = seqs[j];
        seqs[j] = t;
      }
    }
  }
  for (int i = 0; i < n - REPORT_KEEP; i++) {
    char path[48];
    snprintf(path, sizeof path, DIAG_DIR "/rpt_%lu.json", (unsigned long)seqs[i]);
    remove(path);
  }
}

#define RAM_REPORT_SLOTS 3

static char *s_ramReports[RAM_REPORT_SLOTS];
static uint32_t s_ramSeq[RAM_REPORT_SLOTS];
static uint8_t s_ramCount = 0;

static void ramFreeSlot(int slot) {
  if (slot < 0 || slot >= RAM_REPORT_SLOTS) return;
  free(s_ramReports[slot]);
  s_ramReports[slot] = nullptr;
}

static void ramStore(uint32_t bootSeq, const char *json, size_t len) {
  if (len >= REPORT_MAX) len = REPORT_MAX - 1;
  int slot = -1;
  for (uint8_t i = 0; i < s_ramCount; i++) {
    if (s_ramSeq[i] == bootSeq) slot = (int)i;
  }
  if (slot < 0) {
    if (s_ramCount < RAM_REPORT_SLOTS) slot = (int)s_ramCount++;
    else {
      slot = 0;
      ramFreeSlot(slot);
    }
  }
  char *buf = (char *)malloc(len + 1);
  if (!buf) return;
  memcpy(buf, json, len);
  buf[len] = '\0';
  ramFreeSlot(slot);
  s_ramReports[slot] = buf;
  s_ramSeq[(size_t)slot] = bootSeq;
}

bool kerryDiagStoreReport(uint32_t bootSeq, const char *json, size_t len) {
  if (!json || !len) return false;
  if (s_mounted) {
    char path[48];
    snprintf(path, sizeof path, DIAG_DIR "/rpt_%lu.json", (unsigned long)bootSeq);
    if (writeAtomic(path, nullptr, 0, json, len)) {
      pruneReports();
      return true;
    }
  }
  ramStore(bootSeq, json, len);
  return false;
}

bool kerryDiagUpdateReport(uint32_t bootSeq, const char *json, size_t len) {
  return kerryDiagStoreReport(bootSeq, json, len);
}

void kerryDiagListReports(JsonArray out) {
  uint32_t seqs[REPORT_KEEP + 8];
  char paths[REPORT_KEEP + 8][48];
  int n = 0;
  if (s_mounted) {
    DIR *d = opendir(DIAG_DIR);
    if (d) {
      struct dirent *ent;
      while ((ent = readdir(d)) != nullptr && n < (int)(sizeof seqs / sizeof seqs[0])) {
        if (strncmp(ent->d_name, "rpt_", 4) != 0) continue;
        uint32_t seq = (uint32_t)strtoul(ent->d_name + 4, nullptr, 10);
        snprintf(paths[n], sizeof paths[n], DIAG_DIR "/%s", ent->d_name);
        seqs[n++] = seq;
      }
      closedir(d);
    }
  }
  for (uint8_t i = 0; i < s_ramCount && n < (int)(sizeof seqs / sizeof seqs[0]); i++) {
    bool dup = false;
    for (int j = 0; j < n; j++) {
      if (seqs[j] == s_ramSeq[i]) dup = true;
    }
    if (!dup) {
      seqs[n] = s_ramSeq[i];
      paths[n][0] = '\0';
      n++;
    }
  }
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (seqs[i] < seqs[j]) {
        uint32_t ts = seqs[i];
        seqs[i] = seqs[j];
        seqs[j] = ts;
        char tp[48];
        memcpy(tp, paths[i], sizeof tp);
        memcpy(paths[i], paths[j], sizeof paths[j]);
        memcpy(paths[j], tp, sizeof tp);
      }
    }
  }
  for (int i = 0; i < n && i < REPORT_KEEP; i++) {
    JsonDocument one;
    if (paths[i][0]) {
      FILE *f = fopen(paths[i], "rb");
      if (!f) continue;
      char buf[REPORT_MAX];
      size_t r = fread(buf, 1, sizeof buf - 1, f);
      fclose(f);
      buf[r] = '\0';
      if (deserializeJson(one, buf)) continue;
    } else {
      const char *buf = nullptr;
      for (uint8_t k = 0; k < s_ramCount; k++) {
        if (s_ramSeq[k] == seqs[i]) buf = s_ramReports[k];
      }
      if (!buf) continue;
      if (deserializeJson(one, buf)) continue;
    }
    JsonObject o = out.add<JsonObject>();
    o.set(one.as<JsonObjectConst>());
  }
}

void kerryDiagClearReports() {
  if (s_mounted) {
    DIR *d = opendir(DIAG_DIR);
    if (d) {
      struct dirent *ent;
      while ((ent = readdir(d)) != nullptr) {
        if (strncmp(ent->d_name, "rpt_", 4) != 0) continue;
        char path[48];
        snprintf(path, sizeof path, DIAG_DIR "/%s", ent->d_name);
        remove(path);
      }
      closedir(d);
    }
  }
  for (int i = 0; i < RAM_REPORT_SLOTS; i++) ramFreeSlot(i);
  s_ramCount = 0;
}

bool kerryDiagLoadFails(void *data, size_t len) {
  if (!data || len < 2) return false;
  if (s_mounted) {
    FILE *f = fopen(FAILS_PATH, "rb");
    if (f) {
      uint8_t ver = 0;
      if (fread(&ver, 1, 1, f) == 1 && ver == FAILS_VER) {
        size_t r = fread(data, 1, len, f);
        fclose(f);
        return r == len;
      }
      fclose(f);
    }
  }
  return false;
}

bool kerryDiagSaveFails(const void *data, size_t len) {
  if (!data || !len) return false;
  if (!s_mounted) return false;
  uint8_t ver = FAILS_VER;
  return writeAtomic(FAILS_PATH, &ver, 1, data, len);
}
