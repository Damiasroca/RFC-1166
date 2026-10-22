# RFC-1166 — Diagnostics: reboot post-mortem, failure counters, log history

Applies on top of commit `6bda6dd`.

Goal: make every reboot and every upload failure visible in the web UI with its cause, plus a browsable recent log, so the reasons for the frequent reboots can be found.

## 0. Rules

1. Never leave whitespace on blank lines or at the end of lines.
2. No comments addressed to the user or describing "what was changed". Only normal developer comments for non-obvious logic.
3. One item per commit, in the order of section 11. Build after each item.
4. No workarounds or hard-coded hacks. If something does not work as specified, STOP and ask.
5. No changes outside the items listed here.
6. Build scripts: the only allowed change is adding new web files to the `ASSETS` list in `scripts/embed_web.py`, one line per file. Nothing else in `scripts/`.
7. Keep files under ~300 lines. `main.cpp`, `web_server.cpp` and `web/system.html` are already large: new code goes into the new files named below.
8. Both envs (`esp32cam`, `xiao_esp32s3`) must build.

## 1. Facts to verify first (report findings, no code)

| # | Check | Where | Used by |
|---|---|---|---|
| F1 | Whether `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` and `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF` are set in the build | the generated `sdkconfig.h` for each env (build dir) | Item 4 |
| F2 | Whether `partitions_ota.csv` (not in git) has a `coredump` partition, and the size of the `nvs` partition | local `partitions_ota.csv` | Items 4, 5 |
| F3 | Signature of `esp_core_dump_get_summary()` and the fields of `esp_core_dump_summary_t` in this IDF version | `esp_core_dump.h` in the framework package | Item 4 |
| F4 | Full list of `esp_reset_reason_t` values in this IDF version | `esp_system.h` | Item 3 |
| F5 | RTC slow memory used by `.rtc.data`/`.rtc_noinit` before this change | linker map (`firmware.map`) | Item 2 |
| F6 | Whether `kerry_ws_log_write()` and `ws_vprintf()` receive disjoint sets of lines (kerry logs vs ESP-IDF logs) | `ws_log.cpp`, `kerry_log.cpp` | Items 2, 7 |

Core dump (item 4) is optional: if F1 or F2 is missing, implement everything else, compile the core dump code out with `#if`, and report what would be needed. Do not change the partition table or sdkconfig. A partition change cannot be applied over OTA.

## 2. Item 1 — Crash-surviving RTC record (`kerry_postmortem.h/.cpp`)

### 2.1 Layout

One `RTC_NOINIT_ATTR` struct (not zeroed at boot, survives software reset, panic and watchdog resets; lost on power-on):

```c
#define KERRY_PM_MAGIC 0x4B504D31u
#define KERRY_PM_VERSION 1
#define KERRY_PM_LINES 20
#define KERRY_PM_LINE_LEN 96

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t lineHead;
  uint16_t lineCount;
  char lines[KERRY_PM_LINES][KERRY_PM_LINE_LEN];
  uint32_t uptimeS;
  uint32_t epoch;
  uint32_t heapFree;
  uint32_t heapMin;
  uint32_t heapLargest;
  uint32_t internalFree;
  uint32_t internalMin;
  uint32_t psramFree;
  uint32_t psramLargest;
  int8_t rssi;
  uint8_t wifiStatus;
  uint32_t activity;
  char cause[64];
  char fwGit[16];
  char fwVersion[24];
  kerry_stats_session_t stats;
} kerry_pm_rtc_t;
```

`kerry_stats_session_t` is defined by item 5. Confirm with the map file (F5) that the total RTC slow memory use stays below 6 KB; if not, reduce `KERRY_PM_LINES` and report.

### 2.2 Boot

`void kerryPmBoot();` must be the first call in `setup()`, before `kerry_ws_log_hook()` and before any `KERRY_LOG*`:

1. Validate the record: magic, version, `lineHead < KERRY_PM_LINES`, `lineCount <= KERRY_PM_LINES`, every line and string field NUL-terminated within its size.
2. If valid and the reset reason is not power-on, copy the whole record into a heap-allocated `s_prev` (PSRAM if available) for item 3.
3. Re-initialise the record for this session: zero it, set magic/version, `fwGit`/`fwVersion` from `kerry_version`.

### 2.3 Feeding

- `void kerryPmLogLine(const char *line, int len);` — copy up to `KERRY_PM_LINE_LEN - 1` bytes (strip the trailing newline) into the ring under a `portMUX` critical section. Called from `kerry_log_v()` after formatting and from `ws_vprintf()` for ESP-IDF lines (per F6, without duplicates).
- `void kerryPmSetActivity(uint32_t bit, bool on);` with bits `KERRY_ACT_UPLOAD`, `KERRY_ACT_CAPTURE`, `KERRY_ACT_STREAM`, `KERRY_ACT_LIVEPUSH`, `KERRY_ACT_OTA`, `KERRY_ACT_SETTLE`, `KERRY_ACT_ECOWITT`, `KERRY_ACT_CAM_INIT`. Set and clear at the start and every exit of: `uploadCaptureToFinestra()`, `captureHandler()`, the stream handler, live push, the OTA handler, `kerryCamExposureSettle()`, the Ecowitt upload, `startCameraWithPwdn()`.
- `void kerryPmHeartbeat();` — called every 5 s from `loop()` (next to the existing heartbeat): fills uptime, epoch (only if the clock is synced, else 0), the heap fields, RSSI and Wi-Fi status.

### 2.4 Deliberate reboots

- `void kerryRebootWithCause(const char *cause);` — writes `cause` into the record, calls `kerryStatsFlush()` (item 5), logs `rebooting: <cause>`, waits 400 ms, `esp_restart()`.
- Replace every `ESP.restart()` / `esp_restart()` call with it:
  - `kerry_health.cpp` `rebootSoon()`: cause `"watchdog: <detail>"`. Keep the existing 1500 ms delay before the call. Keep the health module's own RTC cap logic unchanged.
  - `web_server.cpp` reboot action: `"user reboot"`.
  - `web_server.cpp` settings save: `"settings saved"`.
  - `main.cpp` OTA completion: `"OTA update"`.
- Search for any other restart call and report it.

## 3. Item 2 — Boot reports

### 3.1 Report content

Built once in `setup()`, after NVS and the clock module are initialised:

- Boot sequence number (NVS counter, incremented every boot).
- Reset reason: full name for every value in F4 (`POWERON`, `EXT`, `SW`, `PANIC`, `INT_WDT`, `TASK_WDT`, `WDT`, `DEEPSLEEP`, `BROWNOUT`, `SDIO`, plus any others present).
- Classification: `deliberate` (SW reset with a cause), `crash` (PANIC, INT_WDT, TASK_WDT, WDT), `power` (POWERON, BROWNOUT), `other`.
- From `s_prev` when present: cause, previous uptime, last known epoch, heap fields, RSSI, Wi-Fi status, activity bits as names, firmware git/version, and the log lines in order (oldest first).
- Core dump summary (item 4) when present.
- Boot time: filled later with the epoch once the clock syncs (update the stored report once).

### 3.2 Storage

- NVS namespace `kpm`. Keep the last 5 reports as blobs `r0`..`r4` with a `head` index; each report serialised as compact JSON (ArduinoJson) capped at 2600 bytes, truncating the log lines from the oldest first if needed.
- If F2 shows an NVS partition smaller than 24 KB, keep 3 reports instead and report it. Check `nvs_get_stats()` free entries before writing; if fewer than needed, skip storing and log a warning.
- Reset-reason counters: one `uint32` per reason name, key `n_<REASON>` (truncate to NVS key length), plus `n_deliberate` / `n_crash` / `n_power`, incremented once per boot.

## 4. Item 3 — Core dump summary (conditional on F1/F2)

Inside `#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF`:

1. At boot, if `esp_core_dump_image_check() == ESP_OK`, call `esp_core_dump_get_summary()`.
2. Store in the boot report: crashed task name, PC, exception cause and fault address (Xtensa fields per F3), backtrace addresses (up to the depth reported, flag if corrupted), app ELF SHA256 prefix.
3. Add a decode hint string: `xtensa-esp32-elf-addr2line -pfiaC -e firmware.elf <PC> <addr...>` (use `xtensa-esp32s3-elf-addr2line` on the S3 env), and the firmware git hash of the session that crashed, so the right `firmware.elf` can be found.
4. Erase the image with `esp_core_dump_image_erase()` after the report is stored, so the next crash is captured fresh.
5. When compiled out, the report contains `"coredump": "not enabled in this build"`.

## 5. Item 4 — Failure counters (`kerry_stats.h/.cpp`)

### 5.1 Counters

```c
typedef struct {
  uint32_t upAttempts, upOk, upSkippedNet, upCaptureFail, upOom, upHttp4xx, upHttp5xx, upHttpOther, upNoConn;
  uint32_t ecoAttempts, ecoOk, ecoFail;
  uint32_t camFail, camReinit;
  uint32_t wdReboots, wdHeld;
} kerry_stats_counts_t;
```

`kerry_stats_session_t` (in the RTC record) holds a `kerry_stats_counts_t` for the session plus `upStreak`, `upLongestStreak`, `lastUpOkEpoch`, `lastUpFailEpoch`, `lastUpFailText[64]`.

### 5.2 Recording

`void kerryStatsNote(kerry_stat_event_t ev, const char *detail, int code);` with events mapped from existing code paths:

| Event | Where |
|---|---|
| upload attempt / ok / skipped (no Wi-Fi or portal) / capture failed / OOM / HTTP code / no connection | `uploadCaptureToFinestra()`, each return path |
| Ecowitt attempt / ok / fail | the Ecowitt upload result paths in `ecowitt.cpp` (where `s_uploadStatus` is set) |
| camera capture failure | `kerryHealthNoteCameraFail()` |
| camera reinit | `startCamera()` retry path in `loop()` and `kerryImageSet()` restart path |
| watchdog reboot / held | `maybeReboot()` in `kerry_health.cpp` |

Updates go to the RTC session counters under a `portMUX` critical section (no NVS write per event).

Recent failures: keep the last 20 failure events (`epoch` or uptime, kind, detail, HTTP code) in a RAM ring guarded by a mutex.

### 5.3 Persistence

- Lifetime counters in NVS namespace `kstats` as one blob (`kerry_stats_counts_t` plus version byte), and the recent-failures ring as a second blob.
- At boot (after `kerryPmBoot()`): if the previous RTC record was valid, add its session counters to the lifetime blob and save; then start the session at zero. This keeps counts across crashes without writing NVS on every event.
- `void kerryStatsFlush();` saves lifetime + current session (adds the session delta since the last flush) and the failures ring. Call it every 10 minutes from `loop()` and from `kerryRebootWithCause()`.
- `const char *kerryStatsReset();` clears lifetime, session and the failures ring.

## 6. Item 5 — Log history (`kerry_log_history.h/.cpp`)

- A line ring with a monotonically increasing sequence number per line. Buffer 64 KB in PSRAM when available, else 8 KB internal.
- Fed from the same two points as `kerryPmLogLine()` (one call site each: add a small `kerryLogSink(const char *line, int len)` in `kerry_log.cpp` that calls both, to avoid duplicating the wiring).
- `size_t kerryLogHistoryRead(uint32_t sinceSeq, char *out, size_t cap, uint32_t *lastSeq, bool *truncated);` returns whole lines with `seq > sinceSeq`, at most `cap` bytes; `truncated` set when `sinceSeq` is older than the oldest kept line.
- Guarded by a mutex taken with a 0 timeout on the write side (drop the line rather than block a logger), like the existing ws ring.

## 7. Item 6 — API (`web_diag_api.h/.cpp`)

`void kerryWebDiagApiRegister(httpd_handle_t server);` called from the registration sequence in `web_server.cpp`. Check `max_uri_handlers` (48) still has headroom and report the count. All routes require auth like `/api/system`.

- `GET /api/postmortem`: current boot (sequence, reason, classification, uptime), reset counters, and the stored reports, newest first.
- `POST /api/postmortem` `{ "action": "clear" }`: delete reports and reset counters.
- `GET /api/stats`: lifetime and session counters (session = since boot), streaks, last OK/fail time and text, recent failures, and the existing `uploadStatus` / Ecowitt status strings.
- `POST /api/stats` `{ "action": "reset" }`.
- `GET /api/log?since=<seq>`: `{ "lines": [...], "lastSeq": n, "truncated": bool }`, at most 16 KB per response (stream with `httpd_resp_send_chunk` if needed).

Add `/api/log` to the quiet list in `kerry_log_http_verbose()` so polling does not flood the log.

## 8. Item 7 — UI (`web/system.html`, new `web/diag.js`)

Add `diag.js` to `ASSETS` in `scripts/embed_web.py` (one line) and load it from `system.html`. All new logic lives in `diag.js`; `system.html` only gets the section markup.

### 8.1 Reboots section

- Counters: total boots, deliberate / crash / power, and per reset reason.
- Table of reports (newest first): boot time, reason, classification badge, cause, previous uptime (human-readable), activity, firmware.
- Expandable per report: heap/PSRAM/RSSI snapshot, last log lines (monospace), core dump block with the decode command and a copy button.
- Buttons: "Download all as text", "Clear history".

### 8.2 Upload statistics section

- Finestra uploads: attempts, OK, success rate, failures by kind, current and longest fail streak, last OK, last failure with text, lifetime and since-boot side by side.
- Ecowitt: attempts, OK, failures, last failure.
- Camera failures, camera reinits, watchdog reboots and held reboots.
- Recent failures table (time, kind, detail, code).
- Button: "Reset counters".

### 8.3 Log section

- Monospace panel showing the log history, polling `/api/log?since=` every 5 s while the page is visible (stop when hidden).
- Controls: pause/resume, clear view (client only), download, a "lines missed" marker when `truncated` is true.
- Keep at most 2000 lines in the DOM.

## 9. Acceptance

- `POST /api/system {"reboot": true}` → a report with classification `deliberate` and cause `user reboot`, with the last log lines.
- A forced crash in a local test build only (e.g. a temporary `abort()` behind a debug endpoint, not committed) → classification `crash`, reason `PANIC`, core dump block when enabled, last log lines up to the crash.
- Unplug and replug power → classification `power`, no previous logs, counters increment.
- Upload failures appear in the counters and recent failures with the correct kind; counts survive a crash and a deliberate reboot.
- The log panel shows new lines within 5 s and never blocks the device when the browser is slow.

## 10. Out of scope

- Changing the partition table or sdkconfig (report if needed).
- Remote/off-device log shipping.

## 11. Implementation order

1. Section 1 verification (no code).
2. Item 4 counters struct only (types needed by the RTC record), then Item 1 RTC record, boot, feeding, `kerryRebootWithCause()`.
3. Item 2 boot reports and reset counters.
4. Item 3 core dump summary (or compiled out).
5. Item 4 counters recording and persistence.
6. Item 5 log history.
7. Item 6 API.
8. Item 7 UI.

After each commit: build both envs, flash the ESP32-CAM, trigger a user reboot, check the System page.
