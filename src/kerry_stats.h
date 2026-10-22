#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

typedef struct {
  uint32_t upAttempts, upOk, upSkippedNet, upCaptureFail, upOom, upHttp4xx, upHttp5xx, upHttpOther, upNoConn;
  uint32_t ecoAttempts, ecoOk, ecoFail;
  uint32_t camFail, camReinit;
  uint32_t wdReboots, wdHeld;
} kerry_stats_counts_t;

typedef struct {
  kerry_stats_counts_t counts;
  uint32_t upStreak;
  uint32_t upLongestStreak;
  uint32_t lastUpOkEpoch;
  uint32_t lastUpFailEpoch;
  char lastUpFailText[64];
} kerry_stats_session_t;

typedef enum {
  KERRY_SE_UP_ATTEMPT,
  KERRY_SE_UP_OK,
  KERRY_SE_UP_SKIP_NET,
  KERRY_SE_UP_CAPTURE_FAIL,
  KERRY_SE_UP_OOM,
  KERRY_SE_UP_HTTP,
  KERRY_SE_UP_NO_CONN,
  KERRY_SE_ECO_ATTEMPT,
  KERRY_SE_ECO_OK,
  KERRY_SE_ECO_FAIL,
  KERRY_SE_CAM_FAIL,
  KERRY_SE_CAM_REINIT,
  KERRY_SE_WD_REBOOT,
  KERRY_SE_WD_HELD,
} kerry_stat_event_t;

void kerryStatsBoot(bool mergePrevSession);
void kerryStatsNote(kerry_stat_event_t ev, const char *detail, int code);
void kerryStatsFlush();
const char *kerryStatsReset();
void kerryStatsFillJson(JsonDocument &doc);
kerry_stats_session_t *kerryStatsSessionPtr();
