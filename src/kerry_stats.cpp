#include "kerry_stats.h"

#include "kerry_diag_fs.h"
#include "kerry_log.h"
#include "kerry_postmortem.h"

#include <Preferences.h>
#include <nvs.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string.h>
#include <time.h>

#define KSTATS_NS "kstats"
#define KSTATS_VER 1
#define FAIL_RING 20

typedef struct {
  uint32_t epoch;
  uint32_t uptimeS;
  char kind[24];
  char detail[64];
  int code;
} kerry_fail_entry_t;

typedef struct {
  uint8_t head;
  uint8_t count;
  kerry_fail_entry_t entries[FAIL_RING];
} kerry_fail_blob_t;

static kerry_stats_counts_t s_lifetime;
static kerry_stats_counts_t s_sinceBoot;
static kerry_fail_entry_t s_fails[FAIL_RING];
static uint8_t s_failHead = 0;
static uint8_t s_failCount = 0;
static SemaphoreHandle_t s_failMu;
static portMUX_TYPE s_statsMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_dirty = false;
static bool s_failsDirty = false;

static uint32_t nowEpoch() {
  time_t t = 0;
  time(&t);
  return (t >= 1600000000) ? (uint32_t)t : 0;
}

static void addCounts(kerry_stats_counts_t *dst, const kerry_stats_counts_t *src) {
  dst->upAttempts += src->upAttempts;
  dst->upOk += src->upOk;
  dst->upSkippedNet += src->upSkippedNet;
  dst->upCaptureFail += src->upCaptureFail;
  dst->upOom += src->upOom;
  dst->upHttp4xx += src->upHttp4xx;
  dst->upHttp5xx += src->upHttp5xx;
  dst->upHttpOther += src->upHttpOther;
  dst->upNoConn += src->upNoConn;
  dst->ecoAttempts += src->ecoAttempts;
  dst->ecoOk += src->ecoOk;
  dst->ecoFail += src->ecoFail;
  dst->camFail += src->camFail;
  dst->camReinit += src->camReinit;
  dst->wdReboots += src->wdReboots;
  dst->wdHeld += src->wdHeld;
}

static void subCounts(kerry_stats_counts_t *dst, const kerry_stats_counts_t *src) {
  dst->upAttempts -= src->upAttempts;
  dst->upOk -= src->upOk;
  dst->upSkippedNet -= src->upSkippedNet;
  dst->upCaptureFail -= src->upCaptureFail;
  dst->upOom -= src->upOom;
  dst->upHttp4xx -= src->upHttp4xx;
  dst->upHttp5xx -= src->upHttp5xx;
  dst->upHttpOther -= src->upHttpOther;
  dst->upNoConn -= src->upNoConn;
  dst->ecoAttempts -= src->ecoAttempts;
  dst->ecoOk -= src->ecoOk;
  dst->ecoFail -= src->ecoFail;
  dst->camFail -= src->camFail;
  dst->camReinit -= src->camReinit;
  dst->wdReboots -= src->wdReboots;
  dst->wdHeld -= src->wdHeld;
}

static bool countsIsZero(const kerry_stats_counts_t *c) {
  kerry_stats_counts_t z = {};
  return memcmp(c, &z, sizeof z) == 0;
}

static bool nvsWriteOk() {
  nvs_stats_t st;
  if (nvs_get_stats(nullptr, &st) != ESP_OK) return true;
  return st.free_entries >= 40;
}

static void loadLifetime() {
  Preferences prefs;
  if (!prefs.begin(KSTATS_NS, true)) return;
  uint8_t ver = prefs.getUChar("ver", 0);
  if (ver == KSTATS_VER) prefs.getBytes("life", &s_lifetime, sizeof s_lifetime);
  prefs.end();
}

static bool saveLifetimeNvs(const kerry_stats_counts_t *life) {
  if (!life) return false;
  if (!nvsWriteOk()) {
    if (kerry_log_every_ms("kstats_nvs_low", 60000)) KERRY_LOGW("kstats", "NVS low, skipping lifetime save");
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(KSTATS_NS, false)) return false;
  prefs.putUChar("ver", KSTATS_VER);
  size_t n = prefs.putBytes("life", life, sizeof *life);
  prefs.end();
  return n == sizeof *life;
}

static void loadFailsBlob() {
  kerry_fail_blob_t blob;
  memset(&blob, 0, sizeof blob);
  if (!kerryDiagLoadFails(&blob, sizeof blob)) return;
  if (blob.count > FAIL_RING) blob.count = FAIL_RING;
  s_failHead = blob.head;
  s_failCount = blob.count;
  memcpy(s_fails, blob.entries, sizeof s_fails);
}

static void saveFailsBlob() {
  if (!s_failsDirty) return;
  kerry_fail_blob_t blob;
  blob.head = s_failHead;
  blob.count = s_failCount;
  memcpy(blob.entries, s_fails, sizeof s_fails);
  if (kerryDiagSaveFails(&blob, sizeof blob)) s_failsDirty = false;
}

static void noteFail(const char *kind, const char *detail, int code) {
  if (!s_failMu) return;
  if (xSemaphoreTake(s_failMu, 0) != pdTRUE) return;
  kerry_fail_entry_t *e = &s_fails[s_failHead];
  e->epoch = nowEpoch();
  e->uptimeS = (uint32_t)(millis() / 1000UL);
  e->code = code;
  strncpy(e->kind, kind ? kind : "?", sizeof e->kind - 1);
  e->kind[sizeof e->kind - 1] = '\0';
  strncpy(e->detail, detail ? detail : "", sizeof e->detail - 1);
  e->detail[sizeof e->detail - 1] = '\0';
  s_failHead = (uint8_t)((s_failHead + 1) % FAIL_RING);
  if (s_failCount < FAIL_RING) s_failCount++;
  s_failsDirty = true;
  xSemaphoreGive(s_failMu);
}

static void applyEvent(kerry_stat_event_t ev, kerry_stats_counts_t *c, kerry_stats_session_t *sess, const char *detail,
                       int code) {
  switch (ev) {
    case KERRY_SE_UP_ATTEMPT:
      c->upAttempts++;
      break;
    case KERRY_SE_UP_OK:
      c->upOk++;
      if (sess) {
        sess->upStreak = 0;
        sess->lastUpOkEpoch = nowEpoch();
      }
      break;
    case KERRY_SE_UP_SKIP_NET:
      c->upSkippedNet++;
      break;
    case KERRY_SE_UP_CAPTURE_FAIL:
      c->upCaptureFail++;
      if (sess) {
        sess->upStreak++;
        if (sess->upStreak > sess->upLongestStreak) sess->upLongestStreak = sess->upStreak;
        sess->lastUpFailEpoch = nowEpoch();
        if (detail) strncpy(sess->lastUpFailText, detail, sizeof sess->lastUpFailText - 1);
      }
      break;
    case KERRY_SE_UP_OOM:
      c->upOom++;
      if (sess) {
        sess->upStreak++;
        if (sess->upStreak > sess->upLongestStreak) sess->upLongestStreak = sess->upStreak;
        sess->lastUpFailEpoch = nowEpoch();
        if (detail) strncpy(sess->lastUpFailText, detail, sizeof sess->lastUpFailText - 1);
      }
      break;
    case KERRY_SE_UP_HTTP:
      if (code >= 400 && code < 500) c->upHttp4xx++;
      else if (code >= 500 && code < 600) c->upHttp5xx++;
      else c->upHttpOther++;
      if (sess) {
        sess->upStreak++;
        if (sess->upStreak > sess->upLongestStreak) sess->upLongestStreak = sess->upStreak;
        sess->lastUpFailEpoch = nowEpoch();
        if (detail) strncpy(sess->lastUpFailText, detail, sizeof sess->lastUpFailText - 1);
      }
      break;
    case KERRY_SE_UP_NO_CONN:
      c->upNoConn++;
      if (sess) {
        sess->upStreak++;
        if (sess->upStreak > sess->upLongestStreak) sess->upLongestStreak = sess->upStreak;
        sess->lastUpFailEpoch = nowEpoch();
        if (detail) strncpy(sess->lastUpFailText, detail, sizeof sess->lastUpFailText - 1);
      }
      break;
    case KERRY_SE_ECO_ATTEMPT:
      c->ecoAttempts++;
      break;
    case KERRY_SE_ECO_OK:
      c->ecoOk++;
      break;
    case KERRY_SE_ECO_FAIL:
      c->ecoFail++;
      break;
    case KERRY_SE_CAM_FAIL:
      c->camFail++;
      break;
    case KERRY_SE_CAM_REINIT:
      c->camReinit++;
      break;
    case KERRY_SE_WD_REBOOT:
      c->wdReboots++;
      break;
    case KERRY_SE_WD_HELD:
      c->wdHeld++;
      break;
  }
}

static void countsJson(const kerry_stats_counts_t *c, JsonObject obj) {
  obj["upAttempts"] = c->upAttempts;
  obj["upOk"] = c->upOk;
  obj["upSkippedNet"] = c->upSkippedNet;
  obj["upCaptureFail"] = c->upCaptureFail;
  obj["upOom"] = c->upOom;
  obj["upHttp4xx"] = c->upHttp4xx;
  obj["upHttp5xx"] = c->upHttp5xx;
  obj["upHttpOther"] = c->upHttpOther;
  obj["upNoConn"] = c->upNoConn;
  obj["ecoAttempts"] = c->ecoAttempts;
  obj["ecoOk"] = c->ecoOk;
  obj["ecoFail"] = c->ecoFail;
  obj["camFail"] = c->camFail;
  obj["camReinit"] = c->camReinit;
  obj["wdReboots"] = c->wdReboots;
  obj["wdHeld"] = c->wdHeld;
}

void kerryStatsBoot(bool mergePrevSession) {
  if (!s_failMu) s_failMu = xSemaphoreCreateMutex();
  memset(&s_sinceBoot, 0, sizeof s_sinceBoot);
  loadLifetime();
  loadFailsBlob();
  kerry_stats_counts_t pendingPrev = {};
  bool restorePrevToSession = false;
  if (mergePrevSession) {
    const kerry_pm_rtc_t *prev = kerryPmPrevious();
    if (prev) {
      kerry_stats_counts_t merged = s_lifetime;
      addCounts(&merged, &prev->stats.counts);
      if (saveLifetimeNvs(&merged)) {
        s_lifetime = merged;
      } else {
        pendingPrev = prev->stats.counts;
        restorePrevToSession = true;
      }
    }
  }
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (sess) {
    memset(sess, 0, sizeof *sess);
    if (restorePrevToSession) {
      sess->counts = pendingPrev;
      s_dirty = true;
    }
  }
}

void kerryStatsNote(kerry_stat_event_t ev, const char *detail, int code) {
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (!sess) return;
  portENTER_CRITICAL(&s_statsMux);
  applyEvent(ev, &sess->counts, sess, detail, code);
  applyEvent(ev, &s_sinceBoot, nullptr, detail, code);
  s_dirty = true;
  portEXIT_CRITICAL(&s_statsMux);
  const char *failKind = nullptr;
  switch (ev) {
    case KERRY_SE_UP_CAPTURE_FAIL:
      failKind = "upload_capture";
      break;
    case KERRY_SE_UP_OOM:
      failKind = "upload_oom";
      break;
    case KERRY_SE_UP_HTTP:
      failKind = "upload_http";
      break;
    case KERRY_SE_UP_NO_CONN:
      failKind = "upload_net";
      break;
    case KERRY_SE_ECO_FAIL:
      failKind = "ecowitt";
      break;
    case KERRY_SE_CAM_FAIL:
      failKind = "camera";
      break;
    default:
      break;
  }
  if (failKind) noteFail(failKind, detail, code);
}

void kerryStatsFlush() {
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (!sess) return;
  kerry_stats_counts_t delta = {};
  kerry_stats_counts_t candidate = {};
  bool dirty = false;
  portENTER_CRITICAL(&s_statsMux);
  dirty = s_dirty;
  if (s_dirty) {
    delta = sess->counts;
    candidate = s_lifetime;
    addCounts(&candidate, &delta);
  }
  portEXIT_CRITICAL(&s_statsMux);
  if (!dirty && !s_failsDirty) return;
  if (dirty) {
    bool ok = saveLifetimeNvs(&candidate);
    if (ok) {
      portENTER_CRITICAL(&s_statsMux);
      subCounts(&sess->counts, &delta);
      s_lifetime = candidate;
      if (countsIsZero(&sess->counts)) s_dirty = false;
      portEXIT_CRITICAL(&s_statsMux);
    }
  }
  saveFailsBlob();
}

const char *kerryStatsReset() {
  memset(&s_lifetime, 0, sizeof s_lifetime);
  memset(&s_sinceBoot, 0, sizeof s_sinceBoot);
  memset(s_fails, 0, sizeof s_fails);
  s_failHead = 0;
  s_failCount = 0;
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (sess) memset(sess, 0, sizeof *sess);
  kerry_stats_counts_t zero = {};
  saveLifetimeNvs(&zero);
  s_failsDirty = true;
  saveFailsBlob();
  portENTER_CRITICAL(&s_statsMux);
  s_dirty = false;
  portEXIT_CRITICAL(&s_statsMux);
  return nullptr;
}

void kerryStatsFillJson(JsonDocument &doc) {
  kerry_stats_session_t snap = {};
  kerry_stats_counts_t rtcDelta = {};
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (sess) {
    portENTER_CRITICAL(&s_statsMux);
    snap = *sess;
    rtcDelta = sess->counts;
    portEXIT_CRITICAL(&s_statsMux);
  }
  kerry_stats_counts_t life = s_lifetime;
  addCounts(&life, &rtcDelta);
  countsJson(&life, doc["lifetime"].to<JsonObject>());
  countsJson(&s_sinceBoot, doc["session"].to<JsonObject>());
  doc["upStreak"] = snap.upStreak;
  doc["upLongestStreak"] = snap.upLongestStreak;
  doc["lastUpOkEpoch"] = snap.lastUpOkEpoch;
  doc["lastUpFailEpoch"] = snap.lastUpFailEpoch;
  doc["lastUpFailText"] = snap.lastUpFailText;
  JsonArray fails = doc["recentFailures"].to<JsonArray>();
  if (s_failMu && xSemaphoreTake(s_failMu, pdMS_TO_TICKS(50)) == pdTRUE) {
    uint8_t n = s_failCount;
    uint8_t start = (uint8_t)((s_failHead + FAIL_RING - n) % FAIL_RING);
    for (uint8_t i = 0; i < n; i++) {
      uint8_t idx = (uint8_t)((start + i) % FAIL_RING);
      const kerry_fail_entry_t *e = &s_fails[idx];
      JsonObject o = fails.add<JsonObject>();
      o["epoch"] = e->epoch;
      o["uptimeS"] = e->uptimeS;
      o["kind"] = e->kind;
      o["detail"] = e->detail;
      o["code"] = e->code;
    }
    xSemaphoreGive(s_failMu);
  }
}
