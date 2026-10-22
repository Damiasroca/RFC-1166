#include "kerry_stats.h"

#include "kerry_postmortem.h"

#include <Preferences.h>

#include <esp_attr.h>
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

static kerry_stats_counts_t s_lifetime;
static kerry_fail_entry_t s_fails[FAIL_RING];
static uint8_t s_failHead = 0;
static uint8_t s_failCount = 0;
static SemaphoreHandle_t s_failMu;
static portMUX_TYPE s_statsMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_dirty = false;

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

static void loadLifetime() {
  Preferences prefs;
  if (!prefs.begin(KSTATS_NS, true)) return;
  uint8_t ver = prefs.getUChar("ver", 0);
  if (ver == KSTATS_VER) prefs.getBytes("life", &s_lifetime, sizeof s_lifetime);
  size_t flen = prefs.getBytesLength("fails");
  if (flen == sizeof s_fails) prefs.getBytes("fails", s_fails, sizeof s_fails);
  s_failHead = prefs.getUChar("fHead", 0);
  s_failCount = prefs.getUChar("fCount", 0);
  if (s_failCount > FAIL_RING) s_failCount = FAIL_RING;
  prefs.end();
}

static void saveLifetime() {
  Preferences prefs;
  if (!prefs.begin(KSTATS_NS, false)) return;
  prefs.putUChar("ver", KSTATS_VER);
  prefs.putBytes("life", &s_lifetime, sizeof s_lifetime);
  prefs.putBytes("fails", s_fails, sizeof s_fails);
  prefs.putUChar("fHead", s_failHead);
  prefs.putUChar("fCount", s_failCount);
  prefs.end();
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
  xSemaphoreGive(s_failMu);
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
  loadLifetime();
  if (mergePrevSession) {
    const kerry_pm_rtc_t *prev = kerryPmPrevious();
    if (prev) {
      addCounts(&s_lifetime, &prev->stats.counts);
      saveLifetime();
    }
  }
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (sess) memset(sess, 0, sizeof *sess);
}

void kerryStatsNote(kerry_stat_event_t ev, const char *detail, int code) {
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (!sess) return;
  portENTER_CRITICAL(&s_statsMux);
  kerry_stats_counts_t *c = &sess->counts;
  switch (ev) {
    case KERRY_SE_UP_ATTEMPT:
      c->upAttempts++;
      break;
    case KERRY_SE_UP_OK:
      c->upOk++;
      sess->upStreak = 0;
      sess->lastUpOkEpoch = nowEpoch();
      break;
    case KERRY_SE_UP_SKIP_NET:
      c->upSkippedNet++;
      break;
    case KERRY_SE_UP_CAPTURE_FAIL:
      c->upCaptureFail++;
      sess->upStreak++;
      if (sess->upStreak > sess->upLongestStreak) sess->upLongestStreak = sess->upStreak;
      sess->lastUpFailEpoch = nowEpoch();
      if (detail) strncpy(sess->lastUpFailText, detail, sizeof sess->lastUpFailText - 1);
      break;
    case KERRY_SE_UP_OOM:
      c->upOom++;
      sess->upStreak++;
      if (sess->upStreak > sess->upLongestStreak) sess->upLongestStreak = sess->upStreak;
      sess->lastUpFailEpoch = nowEpoch();
      if (detail) strncpy(sess->lastUpFailText, detail, sizeof sess->lastUpFailText - 1);
      break;
    case KERRY_SE_UP_HTTP:
      if (code >= 400 && code < 500) c->upHttp4xx++;
      else if (code >= 500 && code < 600) c->upHttp5xx++;
      else c->upHttpOther++;
      sess->upStreak++;
      if (sess->upStreak > sess->upLongestStreak) sess->upLongestStreak = sess->upStreak;
      sess->lastUpFailEpoch = nowEpoch();
      if (detail) strncpy(sess->lastUpFailText, detail, sizeof sess->lastUpFailText - 1);
      break;
    case KERRY_SE_UP_NO_CONN:
      c->upNoConn++;
      sess->upStreak++;
      if (sess->upStreak > sess->upLongestStreak) sess->upLongestStreak = sess->upStreak;
      sess->lastUpFailEpoch = nowEpoch();
      if (detail) strncpy(sess->lastUpFailText, detail, sizeof sess->lastUpFailText - 1);
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
  portEXIT_CRITICAL(&s_statsMux);
  s_dirty = true;
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
  portENTER_CRITICAL(&s_statsMux);
  addCounts(&s_lifetime, &sess->counts);
  memset(&sess->counts, 0, sizeof sess->counts);
  portEXIT_CRITICAL(&s_statsMux);
  saveLifetime();
  s_dirty = false;
}

const char *kerryStatsReset() {
  memset(&s_lifetime, 0, sizeof s_lifetime);
  memset(s_fails, 0, sizeof s_fails);
  s_failHead = 0;
  s_failCount = 0;
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (sess) memset(sess, 0, sizeof *sess);
  saveLifetime();
  s_dirty = false;
  return nullptr;
}

void kerryStatsFillJson(JsonDocument &doc) {
  kerry_stats_session_t snap = {};
  kerry_stats_session_t *sess = kerryStatsSessionPtr();
  if (sess) {
    portENTER_CRITICAL(&s_statsMux);
    snap = *sess;
    portEXIT_CRITICAL(&s_statsMux);
  }
  kerry_stats_counts_t life = s_lifetime;
  addCounts(&life, &snap.counts);
  countsJson(&life, doc["lifetime"].to<JsonObject>());
  countsJson(&snap.counts, doc["session"].to<JsonObject>());
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
