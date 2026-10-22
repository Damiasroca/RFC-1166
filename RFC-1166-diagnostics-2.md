# RFC-1166 — Diagnostics fixes, round 2

Follow-up to `RFC-1166-diagnostics.md`. Applies on top of commit `844bde8`.

## 0. Rules

1. Never leave whitespace on blank lines or at the end of lines.
2. No comments addressed to the user or describing "what was changed". Only normal developer comments for non-obvious logic.
3. One commit per item, in the order of section 10. Build both envs after each item.
4. No workarounds or hard-coded hacks. If something does not work as specified, STOP and ask.
5. No changes outside the items listed here. Do not modify `scripts/`, `partitions_ota.csv` or any sdkconfig.
6. Keep files under ~300 lines; `kerry_pm_reports.cpp` (305) must not grow: new storage code goes in the new file of item 4.

## 1. Known facts (from the user's local files)

`partitions_ota.csv`:

```
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1E0000,
app1,     app,  ota_1,   0x1F0000,0x1E0000,
spiffs,   data, spiffs,  0x3D0000,0x30000,
```

- NVS is 20 KB (5 pages, about 16 KB usable). It already holds the `kerry` and `kautosw` namespaces plus Wi-Fi and PHY data. The diagnostics blobs (up to ~10 KB) must leave NVS.
- The 192 KB `spiffs` partition is not used by the firmware (no SPIFFS/LittleFS code in `src/`). Item 4 uses it for diagnostics files.
- There is no `coredump` partition. Core dump capture is therefore not possible until the partition table changes, which needs a USB flash and is out of scope. Item 5 only makes the report say so clearly and fixes the decode data for when it becomes available.

## 2. Item 1 — Stored XSS in the System page (`web/diag.js`)

### Problem

Log lines, failure details, report causes and other server-provided strings are inserted with `innerHTML` without escaping. Request URIs are logged, so an unauthenticated client can plant markup that runs in the logged-in user's browser.

### Required change

1. Add `function esc(s)` in `diag.js` that converts any value to a string and escapes `&`, `<`, `>`, `"` and `'`.
2. Every value from an API response that is concatenated into an `innerHTML` string goes through `esc()`: report rows, recent failures rows, Finestra and Ecowitt blocks, the detail box (heap line, coredump text, decode hint).
3. The log lines in the report detail box and the coredump JSON are set with `textContent` on dedicated `<pre>` elements created with `document.createElement`, not concatenated.
4. The live log panel already uses `textContent`; keep it.
5. Only static markup and numbers formatted by local helpers may be concatenated unescaped.

### Acceptance

- Requesting `/x<script>alert(1)</script>` from another device, then opening the System page and the report details, shows the text literally and runs nothing.

## 3. Item 2 — `/api/log` memory use and broken JSON (`web_diag_api.cpp`, `kerry_log_history.cpp`)

### Problem

- `static char chunkBuf[16384]` permanently takes 16 KB of internal RAM, which the camera, Wi-Fi and TLS need.
- Each poll builds an ArduinoJson document plus a `String` copy of the whole reply.
- The chunked fallback writes raw log text inside JSON strings; any `"` or `\` breaks the response.
- `kerryLogHistoryRead()` computes `truncated` from `s_oldestSeq` outside the mutex.

### Required change

1. Remove the static buffer. Per request, allocate the read buffer with `heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`; if that fails, fall back to a 4096-byte internal buffer; if that fails too, return HTTP 503. Free it on every return path.
2. Always stream the reply with `httpd_resp_send_chunk`, never building a JSON document or `String`:
   - `{"lines":[`, then each line as a JSON string, comma-separated, then `],"lastSeq":N,"truncated":true|false}`.
   - Add a small `sendJsonString(httpd_req_t *req, const char *s, size_t n)` that writes the opening quote, the text with `"` → `\"`, `\` → `\\`, control characters below 0x20 → `\u00XX`, and the closing quote. Batch output into a small stack buffer (e.g. 256 bytes) to avoid one chunk per character.
3. In `kerryLogHistoryRead()`, compute `*truncated` after taking the mutex.

### Acceptance

- Free internal heap reported on the System page increases by about 16 KB compared to `844bde8`.
- A log line containing `"quoted"` and `C:\path` appears correctly in the log panel, including on the first load of a full 64 KB history.

## 4. Item 3 — "Since boot" counters reset every 10 minutes (`kerry_stats.cpp`)

### Problem

`kerryStatsFlush()` adds the RTC session counts to lifetime and zeroes them. The RTC counts are the delta since the last flush, which is correct for crash recovery, but the API reports them as "session", so the UI shows only the last 10 minutes.

### Required change

1. Add a RAM-only `static kerry_stats_counts_t s_sinceBoot`, zeroed at boot.
2. `kerryStatsNote()` increments the same field in both the RTC delta and `s_sinceBoot`, inside the existing critical section (factor the per-event field selection into one helper that takes a `kerry_stats_counts_t *` so the switch is not duplicated).
3. `kerryStatsFillJson()`: `"session"` = `s_sinceBoot`; `"lifetime"` = stored lifetime + unflushed RTC delta (as today).
4. Streaks and last OK/fail fields stay in the RTC session struct; flush must not clear them (it already does not; keep it that way).

### Acceptance

- After 25 minutes of uptime with uploads every minute, `session.upAttempts` is about 25, not below 10.

## 5. Item 4 — Move diagnostics storage out of NVS (`kerry_diag_fs.h/.cpp`, new)

### 5.1 Filesystem

1. Mount LittleFS (Arduino `LittleFS`) on the partition labelled `spiffs`, base path `/littlefs`, with format-on-fail enabled. Confirm in the Arduino core that `LittleFS.begin(true, "/littlefs", 10, "spiffs")` (or the equivalent signature in this core version) targets that label; if it cannot, STOP and ask.
2. `bool kerryDiagFsReady();` exposes the mount state. If mounting fails, diagnostics keep working in RAM only and the API reports `"storage": "unavailable"`.
3. Directory `/diag`. All writes go to `<name>.tmp`, then `rename()` over the target, so a crash mid-write never leaves a corrupt file.

### 5.2 Boot order (`main.cpp` `setup()`, `kerry_postmortem.cpp`)

`kerryPmBoot()` must stay first, because any log line before it would overwrite the previous session's RTC lines. Required order:

1. `kerryPmBoot()` (no longer calls `kerryStatsBoot()`).
2. `kerryLogHistoryInit()`.
3. Existing early setup, unchanged.
4. `kerryDiagFsInit()` (mount), before `kerryHealthInit()`.
5. `kerryStatsBoot(kerryPmHadValidPrevious())`.
6. `kerryPmReportsInit()` as today.

### 5.3 What moves where

| Data | Before | After |
|---|---|---|
| Boot reports | NVS `kpm/r0..r2`, `head` | `/diag/rpt_<bootSeq>.json`, keep the newest 10 (delete older files by sequence) |
| Recent failures ring | NVS `kstats/fails`, `fHead`, `fCount` | `/diag/fails.bin` with a version byte |
| Lifetime counters | NVS `kstats/life` | stays in NVS (small) |
| Boot sequence and reset counters | NVS `kpm` | stays in NVS (small) |

- Report storage and listing code moves from `kerry_pm_reports.cpp` into `kerry_diag_fs.cpp` (`kerryDiagStoreReport()`, `kerryDiagUpdateReport()` for the clock-sync update, `kerryDiagListReports()` filling a `JsonArray` newest first, `kerryDiagClearReports()`).
- Migration, once at boot after mounting: remove NVS keys `kpm/r0`..`kpm/r4`, `kpm/head`, `kstats/fails`, `kstats/fHead`, `kstats/fCount` if present (`Preferences::remove`). No data conversion needed.
- If the filesystem is unavailable, keep the current report and failures ring in RAM only; do not fall back to NVS.

### 5.4 Write frequency

1. `kerryStatsFlush()` writes only when something changed since the last flush (use the existing `s_dirty`, set it under the critical section, and clear it only after a successful write). Write `/diag/fails.bin` only when the ring changed (separate flag).
2. Before any NVS write in the diagnostics code, check `nvs_get_stats(NULL, &st)`; if `st.free_entries < 40`, skip the write and log a rate-limited warning.

### Acceptance

- After a few reboots, `nvs_get_stats` free entries (log it once at boot) is clearly higher than on `844bde8`.
- Reports and recent failures survive a reboot and a crash.
- The System page shows up to 10 reports.

## 6. Item 5 — Core dump status and decode data (`kerry_pm_reports.cpp`)

1. Report the core dump state precisely:
   - `"not enabled in this build"` when the config macros are off.
   - `"no coredump partition"` when enabled but `esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL)` returns null.
   - `"none"` when the partition exists but holds no image.
2. When an image exists:
   - Add the exception cause and fault address from the Xtensa `ex_info` fields of `esp_core_dump_summary_t` (confirm field names in this IDF version).
   - The decode hint lists the PC followed by every backtrace address: `... -e firmware.elf 0x<pc> 0x<bt0> 0x<bt1> ...`.
   - `fwGit` comes from the crashed session (`prev->fwGit`), falling back to `"unknown"` when there is no valid previous record.
   - Erase the image only after the report has been stored successfully.

## 7. Item 6 — ESP-IDF lines missing from the crash record and history (`ws_log.cpp`)

### Problem

`ws_vprintf()` feeds the crash record and history only while the WebSocket log is running (`s_run`) and only for lines that pass its level filter and `skip_send()`. Early boot and filtered driver messages are lost. It also calls the two sinks directly instead of `kerryLogSink()`.

### Required change

1. Format the line unconditionally at the top of `ws_vprintf()` (into the existing `line` buffer).
2. Call `kerryLogSink(line, n)` for every formatted line, before the `s_run`, level and `skip_send()` checks.
3. Keep the existing WebSocket behaviour (ring, level filter, `skip_send()`, serial output) exactly as it is.
4. Declare `kerryLogSink()` in `kerry_log.h` if it is not already.

### Acceptance

- ESP-IDF warnings printed during early boot (before the WebSocket server starts) appear in the log panel and in the next boot report.

## 8. Item 7 — Report order

`/api/postmortem` returns reports newest first (handled by `kerryDiagListReports()` in item 4). No client-side sorting.

## 9. Item 8 — Small fixes

1. `kerryRebootWithCause()`: log `KERRY_LOGW("main", "rebooting: %s", cause)` (instead of the Serial-only print) before `kerryStatsFlush()`, so the line reaches the crash record and the log history; keep `Serial.flush()` and the delay.
2. `kerryPmBoot()`: validate `s_rtc` in place and copy it straight into the heap allocation; remove the 2.3 KB `scratch` stack copy.

## 10. Implementation order

1. Item 1 — XSS.
2. Item 2 — `/api/log` buffer and streaming.
3. Item 3 — since-boot counters.
4. Item 4 — LittleFS storage, boot order, migration, write frequency.
5. Item 7 — report order (if not already covered by item 4's commit, a separate commit).
6. Item 5 — core dump status and decode data.
7. Item 6 — ESP-IDF lines.
8. Item 8 — small fixes.

After each commit: build both envs, flash the ESP32-CAM, trigger a user reboot, check the System page (reports, counters, log panel).
