(function () {
  var API_TIMEOUT = 10000;
  var LOG_KEEP_LINES = 2000;
  var ANSI_RE = /\x1b\[[0-9;]*m/g;

  var logSeq = 0;
  var logPaused = false;
  var logLines = [];
  var logMissed = false;
  var logMode = "connecting";
  var logWs = null;
  var logWsRetry = null;
  var logPoll = null;
  var statsPoll = null;
  var pmPoll = null;
  var lastUptime = null;
  var reports = [];
  var openReportSeq = null;
  var reportCache = {};
  var tileRefs = {};

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = text;
    return node;
  }

  function tile(label, value, sub, state) {
    var t = el("div", "tile");
    t.appendChild(el("div", "tile-label", label));
    var val = el("div", "tile-value", value != null ? String(value) : "—");
    if (state) val.classList.add(state);
    t.appendChild(val);
    t.appendChild(el("div", "tile-sub", sub || ""));
    return t;
  }

  function badge(text, state) {
    var b = el("span", "badge", text);
    if (state) b.classList.add(state);
    return b;
  }

  function fmtInt(n) {
    if (n === null || n === undefined) return "—";
    return Number(n).toLocaleString();
  }

  function fmtUptime(sec) {
    if (sec === null || sec === undefined || sec < 0) return "—";
    sec = Math.floor(sec);
    var d = Math.floor(sec / 86400);
    sec %= 86400;
    var h = Math.floor(sec / 3600);
    sec %= 3600;
    var m = Math.floor(sec / 60);
    var s = sec % 60;
    if (d > 0) return d + " d " + h + " h";
    if (h > 0) return h + " h " + m + " min";
    if (m > 0) return m + " min";
    return s + " s";
  }

  function pad2(n) {
    return n < 10 ? "0" + n : String(n);
  }

  function fmtAbsTime(ep) {
    if (!ep) return "—";
    var d = new Date(ep * 1000);
    if (isNaN(d.getTime())) return "—";
    return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate()) + " " +
      pad2(d.getHours()) + ":" + pad2(d.getMinutes());
  }

  function fmtRelTime(ep) {
    if (!ep) return "—";
    var now = Math.floor(Date.now() / 1000);
    var diff = now - ep;
    if (diff < 0) diff = 0;
    if (diff < 60) return diff + " s ago";
    if (diff < 3600) return Math.floor(diff / 60) + " min ago";
    if (diff < 86400) return Math.floor(diff / 3600) + " h ago";
    return Math.floor(diff / 86400) + " d ago";
  }

  function badgeState(cls) {
    if (cls === "crash") return "err";
    if (cls === "deliberate") return "ok";
    if (cls === "power") return "warn";
    return undefined;
  }

  function ensureTileGroup(parentId, groupKey, defs) {
    var parent = document.getElementById(parentId);
    if (!parent) return;
    if (!tileRefs[groupKey]) tileRefs[groupKey] = {};
    defs.forEach(function (def) {
      if (tileRefs[groupKey][def.key]) return;
      var t = tile(def.label, "—", "");
      parent.appendChild(t);
      tileRefs[groupKey][def.key] = {
        value: t.querySelector(".tile-value"),
        sub: t.querySelector(".tile-sub")
      };
    });
  }

  function setTile(groupKey, key, value, sub, state) {
    var ref = tileRefs[groupKey] && tileRefs[groupKey][key];
    if (!ref) return;
    ref.value.textContent = value != null ? String(value) : "—";
    ref.sub.textContent = sub || "";
    ref.value.classList.remove("ok", "warn", "err", "muted");
    if (state) ref.value.classList.add(state);
  }

  function uploadFailSum(c) {
    c = c || {};
    return (c.upCaptureFail || 0) + (c.upOom || 0) + (c.upHttp4xx || 0) + (c.upHttp5xx || 0) +
      (c.upHttpOther || 0) + (c.upNoConn || 0);
  }

  function successRate(ok, att) {
    if (!att) return { text: "—", state: undefined };
    var pct = Math.round((ok / att) * 100);
    var state = "ok";
    if (pct < 80) state = "err";
    else if (pct < 95) state = "warn";
    return { text: pct + "%", state: state };
  }

  function failKindLabel(kind) {
    switch (kind) {
      case "upload_capture": return "Upload · capture";
      case "upload_oom": return "Upload · memory";
      case "upload_http": return "Upload · HTTP";
      case "upload_net": return "Upload · network";
      case "ecowitt": return "Ecowitt";
      case "camera": return "Camera";
      default: return kind || "—";
    }
  }

  function wifiStatusText(code) {
    if (code === null || code === undefined) return "—";
    if (code === 3) return "Connected";
    if (code === 0) return "Idle";
    return "Status " + code;
  }

  function initTileLayouts() {
    ensureTileGroup("diagPmSummaryTiles", "pmSummary", [
      { key: "recorded", label: "Boots recorded" },
      { key: "crashes", label: "Crashes" },
      { key: "deliberate", label: "Deliberate" },
      { key: "power", label: "Power" },
      { key: "thisBoot", label: "This boot" }
    ]);
    ensureTileGroup("diagFinestraTiles", "finestra", [
      { key: "attempts", label: "Attempts" },
      { key: "success", label: "Success rate" },
      { key: "failed", label: "Failed" },
      { key: "streak", label: "Fail streak" },
      { key: "lastOk", label: "Last success" },
      { key: "lastFail", label: "Last failure" }
    ]);
    ensureTileGroup("diagFailKindTiles", "failKind", [
      { key: "skipped", label: "No network" },
      { key: "capture", label: "Capture failed" },
      { key: "oom", label: "Out of memory" },
      { key: "http4", label: "HTTP 4xx" },
      { key: "http5", label: "HTTP 5xx" },
      { key: "httpOther", label: "HTTP other" },
      { key: "noConn", label: "No connection" }
    ]);
    ensureTileGroup("diagEcowittTiles", "ecowitt", [
      { key: "attempts", label: "Attempts" },
      { key: "ok", label: "OK" },
      { key: "fail", label: "Failed" }
    ]);
    ensureTileGroup("diagCamWdTiles", "camWd", [
      { key: "camFail", label: "Camera failures" },
      { key: "camReinit", label: "Camera restarts" },
      { key: "wdReboot", label: "Watchdog reboots" },
      { key: "wdHeld", label: "Watchdog reboots held back" }
    ]);
  }

  function paintReasonTiles(byReason) {
    var parent = document.getElementById("diagReasonTiles");
    if (!parent) return;
    parent.textContent = "";
    var keys = Object.keys(byReason || {}).sort();
    if (!keys.length) {
      parent.appendChild(el("p", "hint", "No reason counters yet."));
      return;
    }
    keys.forEach(function (k) {
      parent.appendChild(tile(k, fmtInt(byReason[k]), ""));
    });
  }

  function paintReportsTable(list) {
    var tbody = document.getElementById("diagReportsBody");
    if (!tbody) return;
    tbody.textContent = "";
    if (!list.length) {
      var tr0 = el("tr");
      var td0 = el("td", "empty", "No reboot reports yet");
      td0.colSpan = 8;
      tr0.appendChild(td0);
      tbody.appendChild(tr0);
      return;
    }
    list.forEach(function (r) {
      var tr = el("tr");
      var whenTd = el("td");
      if (r.bootTime) {
        whenTd.textContent = fmtAbsTime(r.bootTime);
      } else {
        whenTd.appendChild(el("span", "hint", "Boot #" + (r.bootSeq != null ? r.bootSeq : "?")));
      }
      tr.appendChild(whenTd);
      tr.appendChild(el("td", null, r.reason || "—"));
      var clsTd = el("td");
      clsTd.appendChild(badge(r.class || "?", badgeState(r.class)));
      tr.appendChild(clsTd);
      tr.appendChild(el("td", null, r.cause || "—"));
      tr.appendChild(el("td", null, fmtUptime(r.prevUptimeS)));
      tr.appendChild(el("td", "col-opt", (r.activity || []).join(", ") || "—"));
      tr.appendChild(el("td", "col-opt", r.fwGit || r.fwVersion || "—"));
      var actTd = el("td");
      var btn = el("button", "common-btn btn-secondary btn-sm", "Details");
      btn.type = "button";
      btn.dataset.rpt = String(r.bootSeq);
      actTd.appendChild(btn);
      tr.appendChild(actTd);
      tbody.appendChild(tr);
    });
  }

  function coredumpText(cd) {
    if (typeof cd === "string") return cd;
    if (!cd) return "—";
    return JSON.stringify(cd, null, 2);
  }

  function detailBox() {
    return document.getElementById("diagReportDetail");
  }

  function paintReportMessage(text) {
    var box = detailBox();
    if (!box) return;
    box.textContent = "";
    box.hidden = false;
    box.appendChild(el("p", "hint", text));
  }

  function paintReportDetail(r, savedLog) {
    var box = detailBox();
    if (!box) return;
    box.textContent = "";
    if (!r) {
      box.hidden = true;
      return;
    }
    box.hidden = false;
    box.appendChild(el("p", null, "Boot #" + (r.bootSeq != null ? r.bootSeq : "?") + " · " +
      (r.reason || "—") + " · " + (r.class || "—")));
    var grid = el("div", "tiles");
    grid.appendChild(tile("Heap free", fmtBytes(r.heapFree), ""));
    grid.appendChild(tile("Heap min", fmtBytes(r.heapMin), ""));
    grid.appendChild(tile("Internal free", fmtBytes(r.internalFree), ""));
    grid.appendChild(tile("Internal min", fmtBytes(r.internalMin), ""));
    grid.appendChild(tile("PSRAM free", fmtBytes(r.psramFree), ""));
    grid.appendChild(tile("Largest PSRAM block", fmtBytes(r.psramLargest), ""));
    var rssiSub = r.rssi != null && r.rssi !== 0 ? r.rssi + " dBm" : "not connected";
    grid.appendChild(tile("RSSI", rssiSub, wifiStatusText(r.wifiStatus)));
    box.appendChild(grid);
    if (savedLog) {
      box.appendChild(el("h3", "tile-group", "Log before reboot"));
      box.appendChild(el("pre", "log-panel", savedLog.replace(ANSI_RE, "")));
    } else {
      box.appendChild(el("h3", "tile-group", "Last log lines"));
      var lines = r.prevLines || [];
      if (lines.length) {
        box.appendChild(el("pre", "mono-block", lines.join("\n")));
      } else {
        box.appendChild(el("p", "hint", "No log lines (power loss clears them)"));
      }
    }
    box.appendChild(el("h3", "tile-group", "Core dump"));
    box.appendChild(el("pre", "mono-block", coredumpText(r.coredump)));
    var hint = r.coredump && typeof r.coredump === "object" ? r.coredump.decodeHint : "";
    if (hint) {
      var row = el("div", "btn-row");
      var copyBtn = el("button", "common-btn btn-secondary btn-sm", "Copy decode command");
      copyBtn.type = "button";
      copyBtn.addEventListener("click", function () {
        navigator.clipboard.writeText(hint).catch(function () {});
        toast("Copied", "ok");
      });
      row.appendChild(copyBtn);
      box.appendChild(row);
    }
  }

  function fetchText(url, cb) {
    var xhr = new XMLHttpRequest();
    xhr.timeout = API_TIMEOUT;
    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4) return;
      cb(xhr.status >= 200 && xhr.status < 300 ? xhr.responseText : null);
    };
    xhr.open("GET", url, true);
    xhr.send();
  }

  // Full report, plus the log saved before a deliberate reboot when there is one.
  function fetchReport(seq, hasLog, cb) {
    if (reportCache[seq]) {
      cb(reportCache[seq]);
      return;
    }
    ajax({
      url: "/api/postmortem/report?seq=" + seq,
      timeout: API_TIMEOUT,
      success: function (rep) {
        var entry = { report: rep, log: null };
        if (!hasLog) {
          reportCache[seq] = entry;
          cb(entry);
          return;
        }
        fetchText("/api/postmortem/log?seq=" + seq, function (text) {
          entry.log = text;
          if (text !== null) reportCache[seq] = entry;
          cb(entry);
        });
      },
      error: function () { cb(null); }
    });
  }

  function findSummary(seq) {
    for (var i = 0; i < reports.length; i++) {
      if (reports[i].bootSeq === seq) return reports[i];
    }
    return null;
  }

  function openReport(seq) {
    openReportSeq = seq;
    var summary = findSummary(seq);
    paintReportMessage("Loading boot #" + seq + "…");
    fetchReport(seq, !!(summary && summary.hasLog), function (entry) {
      if (openReportSeq !== seq) return;
      if (!entry) {
        paintReportMessage("The camera did not answer. Try again in a moment.");
        return;
      }
      paintReportDetail(entry.report, entry.log);
    });
  }

  function paintReboots(data) {
    var ctr = data.counters || {};
    var subBoot = "Current boot #" + fmtInt(data.bootSeq || 0) + ", up " + fmtUptime(data.uptimeS);
    setTile("pmSummary", "recorded", fmtInt(ctr.total || 0), subBoot);
    setTile("pmSummary", "crashes", fmtInt(ctr.crash || 0), "", (ctr.crash || 0) > 0 ? "err" : undefined);
    setTile("pmSummary", "deliberate", fmtInt(ctr.deliberate || 0), "Watchdog, user, settings, OTA");
    setTile("pmSummary", "power", fmtInt(ctr.power || 0), "Power-on or brownout", (ctr.power || 0) > 0 ? "warn" : undefined);
    setTile("pmSummary", "thisBoot", data.reason || "—", data.class || "—");
    paintReasonTiles(ctr.byReason || {});
    // The current boot's report gains its boot time after the clock syncs.
    if (data.bootSeq != null) delete reportCache[data.bootSeq];
    reports = data.reports || [];
    paintReportsTable(reports);
    if (openReportSeq !== null && !findSummary(openReportSeq)) {
      openReportSeq = null;
      paintReportDetail(null);
    }
  }

  function sinceSub(life, sess, field) {
    var s = (sess && sess[field]) || 0;
    if (!s) return "";
    return fmtInt(s) + " since boot";
  }

  function countTileState(n) {
    return n > 0 ? undefined : "muted";
  }

  function paintStats(res) {
    var life = res.lifetime || {};
    var sess = res.session || {};
    var notice = document.getElementById("diagUploadNotice");
    if (notice) {
      var streak = res.upStreak || 0;
      if (streak > 0) {
        notice.hidden = false;
        notice.className = "notice" + (streak >= 3 ? " err" : "");
        notice.textContent = streak + " uploads failed in a row. Last error: " + (res.lastUpFailText || "—");
      } else {
        notice.hidden = true;
        notice.textContent = "";
      }
    }
    var rate = successRate(life.upOk || 0, life.upAttempts || 0);
    setTile("finestra", "attempts", fmtInt(life.upAttempts || 0), sinceSub(life, sess, "upAttempts"));
    setTile("finestra", "success", rate.text, sinceSub(life, sess, "upOk"), rate.state);
    var failed = uploadFailSum(life);
    var failedSess = uploadFailSum(sess);
    setTile("finestra", "failed", fmtInt(failed), failedSess ? fmtInt(failedSess) + " since boot" : "", failed > 0 ? "err" : undefined);
    setTile("finestra", "streak", fmtInt(res.upStreak || 0), "longest " + fmtInt(res.upLongestStreak || 0));
    setTile("finestra", "lastOk", fmtRelTime(res.lastUpOkEpoch), fmtAbsTime(res.lastUpOkEpoch));
    setTile("finestra", "lastFail", fmtRelTime(res.lastUpFailEpoch), res.lastUpFailText || "—");

    function kindTile(key, label, field) {
      var v = life[field] || 0;
      setTile("failKind", key, fmtInt(v), sinceSub(life, sess, field), countTileState(v));
    }
    kindTile("skipped", "No network", "upSkippedNet");
    kindTile("capture", "Capture failed", "upCaptureFail");
    kindTile("oom", "Out of memory", "upOom");
    kindTile("http4", "HTTP 4xx", "upHttp4xx");
    kindTile("http5", "HTTP 5xx", "upHttp5xx");
    kindTile("httpOther", "HTTP other", "upHttpOther");
    kindTile("noConn", "No connection", "upNoConn");

    setTile("ecowitt", "attempts", fmtInt(life.ecoAttempts || 0), sinceSub(life, sess, "ecoAttempts"));
    setTile("ecowitt", "ok", fmtInt(life.ecoOk || 0), sinceSub(life, sess, "ecoOk"));
    var ecoFail = life.ecoFail || 0;
    setTile("ecowitt", "fail", fmtInt(ecoFail), sinceSub(life, sess, "ecoFail"), ecoFail > 0 ? "err" : countTileState(ecoFail));
    var ecoHint = document.getElementById("diagEcowittHint");
    if (ecoHint) ecoHint.textContent = res.ecowittStatus || "—";

    setTile("camWd", "camFail", fmtInt(life.camFail || 0), sinceSub(life, sess, "camFail"), countTileState(life.camFail));
    setTile("camWd", "camReinit", fmtInt(life.camReinit || 0), sinceSub(life, sess, "camReinit"), countTileState(life.camReinit));
    setTile("camWd", "wdReboot", fmtInt(life.wdReboots || 0), sinceSub(life, sess, "wdReboots"), countTileState(life.wdReboots));
    setTile("camWd", "wdHeld", fmtInt(life.wdHeld || 0), sinceSub(life, sess, "wdHeld"), countTileState(life.wdHeld));

    var ftbody = document.getElementById("diagFailBody");
    if (!ftbody) return;
    ftbody.textContent = "";
    var fails = (res.recentFailures || []).slice().reverse();
    if (!fails.length) {
      var tr0 = el("tr");
      var td0 = el("td", "empty", "No failures recorded");
      td0.colSpan = 4;
      tr0.appendChild(td0);
      ftbody.appendChild(tr0);
      return;
    }
    fails.forEach(function (f) {
      var tr = el("tr");
      tr.appendChild(el("td", null, f.epoch ? fmtAbsTime(f.epoch) : fmtUptime(f.uptimeS)));
      tr.appendChild(el("td", null, failKindLabel(f.kind)));
      tr.appendChild(el("td", null, f.detail || "—"));
      var codeTd = el("td", "num", f.code !== undefined && f.code >= 0 ? String(f.code) : "—");
      tr.appendChild(codeTd);
      ftbody.appendChild(tr);
    });
  }

  function loadPostmortem(done) {
    ajax({
      url: "/api/postmortem",
      timeout: API_TIMEOUT,
      success: function (res) {
        try {
          paintReboots(res);
        } finally {
          done(true);
        }
      },
      error: function () { done(false); }
    });
  }

  function loadStats(done) {
    ajax({
      url: "/api/stats",
      timeout: API_TIMEOUT,
      success: function (res) {
        try {
          paintStats(res);
        } finally {
          done(true);
        }
      },
      error: function () { done(false); }
    });
  }

  function updateLogStatus() {
    var st = document.getElementById("diagLogStatus");
    if (!st) return;
    var text;
    if (logPaused) text = "Paused";
    else if (logMode === "ws") text = "Live";
    else if (logMode === "poll") text = "Live · polling every 3 s";
    else if (logMode === "down") text = "Camera not responding · retrying";
    else text = "Connecting…";
    if (logMissed && !logPaused) text += " · some lines were missed";
    st.textContent = text;
  }

  function renderLogPanel() {
    updateLogStatus();
    if (logPaused) return;
    var pre = document.getElementById("diagLogPanel");
    if (!pre) return;
    var atBottom = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 12;
    var text = logLines.join("\n");
    if (logMissed && text) text = "— lines missed —\n" + text;
    pre.textContent = text;
    if (atBottom) pre.scrollTop = pre.scrollHeight;
  }

  function appendLogLine(line) {
    line = String(line).replace(ANSI_RE, "").replace(/\r$/, "");
    if (line) logLines.push(line);
  }

  function trimLog() {
    if (logLines.length > LOG_KEEP_LINES) logLines.splice(0, logLines.length - LOG_KEEP_LINES);
  }

  // Newest lines first time round, then everything after logSeq until the camera says no more.
  function fetchLog(done) {
    ajax({
      url: "/api/log?since=" + logSeq + (logSeq ? "" : "&tail=1"),
      timeout: API_TIMEOUT,
      success: function (res) {
        if (res.truncated) logMissed = true;
        (res.lines || []).forEach(appendLogLine);
        trimLog();
        if (res.lastSeq !== undefined) logSeq = res.lastSeq;
        renderLogPanel();
        if (res.more) fetchLog(done);
        else done(true);
      },
      error: function () { done(false); }
    });
  }

  function pollLogTask(done) {
    fetchLog(function (ok) {
      if (logMode !== "ws") logMode = ok ? "poll" : "down";
      updateLogStatus();
      done(ok);
    });
  }

  // WebSocket lines carry no sequence number, so after a drop reload the tail rather
  // than polling from a stale logSeq and repeating lines.
  function usePolling(resync) {
    if (resync) {
      logLines = [];
      logSeq = 0;
      logMissed = false;
    }
    if (logMode === "ws" || logMode === "connecting") logMode = "poll";
    updateLogStatus();
    logPoll.start();
    logPoll.now();
  }

  function connectLogWs() {
    clearTimeout(logWsRetry);
    if (logWs) return;
    if (!window.WebSocket) {
      usePolling(false);
      return;
    }
    var ws;
    try {
      ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.hostname + ":82/ws/log");
    } catch (e) {
      usePolling(false);
      return;
    }
    logWs = ws;
    var opened = false;
    var openTimer = setTimeout(function () { if (!opened) ws.close(); }, 4000);
    ws.onopen = function () {
      opened = true;
      clearTimeout(openTimer);
      logPoll.stop();
      logMode = "ws";
      updateLogStatus();
      fetchLog(function () {});
    };
    ws.onmessage = function (ev) {
      String(ev.data).split("\n").forEach(appendLogLine);
      trimLog();
      renderLogPanel();
    };
    ws.onclose = function () {
      clearTimeout(openTimer);
      if (logWs !== ws) return;
      logWs = null;
      usePolling(opened);
      logWsRetry = setTimeout(connectLogWs, opened ? 5000 : 30000);
    };
  }

  function downloadText(text, filename, type) {
    var blob = new Blob([text], { type: type || "text/plain" });
    var a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = filename;
    a.click();
  }

  document.addEventListener("click", function (ev) {
    var t = ev.target;
    if (!t || !t.dataset || t.dataset.rpt === undefined) return;
    var seq = parseInt(t.dataset.rpt, 10);
    if (isNaN(seq) || !findSummary(seq)) return;
    if (openReportSeq === seq) {
      openReportSeq = null;
      paintReportDetail(null);
    } else {
      openReport(seq);
    }
  });

  // Called with each /api/system response; a drop in uptime means the camera rebooted.
  window.diagNoteSystem = function (res) {
    var up = res && res.mem ? res.mem.uptime_s : undefined;
    if (up === undefined || up === null) return;
    if (lastUptime !== null && up + 5 < lastUptime) {
      logLines.push("— camera rebooted —");
      logSeq = 0;
      renderLogPanel();
      if (pmPoll) pmPoll.now();
      if (statsPoll) statsPoll.now();
      if (!logWs) connectLogWs();
    }
    lastUptime = up;
  };

  window.diagClearHistory = function () {
    if (!confirm("Clear reboot history and counters?")) return;
    ajax({
      method: "POST",
      url: "/api/postmortem",
      timeout: API_TIMEOUT,
      data: { action: "clear" },
      success: function () {
        openReportSeq = null;
        reportCache = {};
        paintReportDetail(null);
        pmPoll.now();
        toast("Cleared", "ok");
      },
      error: function () { toast("The camera did not answer", "err"); }
    });
  };

  // One report at a time: the camera serves a single request at once.
  window.diagDownloadReports = function () {
    ajax({
      url: "/api/postmortem",
      timeout: API_TIMEOUT,
      success: function (summary) {
        var list = summary.reports || [];
        var out = [];
        var i = 0;
        toast("Collecting " + list.length + " reports…", "");
        (function next() {
          if (i >= list.length) {
            downloadText(JSON.stringify({ summary: summary, reports: out }, null, 2), "kerry-reboot-reports.json");
            toast("Downloaded " + out.length + " reports", "ok");
            return;
          }
          var r = list[i++];
          fetchReport(r.bootSeq, !!r.hasLog, function (entry) {
            if (entry) out.push({ report: entry.report, logBeforeReboot: entry.log });
            else out.push({ summary: r, error: "fetch failed" });
            next();
          });
        })();
      },
      error: function () { toast("The camera did not answer", "err"); }
    });
  };

  window.diagResetStats = function () {
    if (!confirm("Reset upload statistics?")) return;
    ajax({
      method: "POST",
      url: "/api/stats",
      timeout: API_TIMEOUT,
      data: { action: "reset" },
      success: function () {
        statsPoll.now();
        toast("Counters reset", "ok");
      },
      error: function () { toast("The camera did not answer", "err"); }
    });
  };

  window.diagToggleLogPause = function () {
    logPaused = !logPaused;
    var btn = document.getElementById("diagLogPauseBtn");
    if (btn) btn.textContent = logPaused ? "Resume" : "Pause";
    renderLogPanel();
  };

  window.diagClearLogView = function () {
    logLines = [];
    logMissed = false;
    renderLogPanel();
  };

  // The camera's whole history buffer, not just what this page has seen.
  window.diagDownloadLog = function () {
    fetchText("/api/log?format=text", function (text) {
      if (text === null) {
        toast("The camera did not answer; saving the lines shown instead", "warn");
        text = logLines.join("\n");
      }
      downloadText(text.replace(ANSI_RE, ""), "kerry-log.txt");
    });
  };

  window.diagInit = function () {
    initTileLayouts();
    logPoll = poller(3000, pollLogTask);
    statsPoll = poller(15000, loadStats);
    pmPoll = poller(120000, loadPostmortem);
    updateLogStatus();
    // Log first; the rest queues behind it on the camera anyway.
    fetchLog(function (ok) {
      if (!ok) logMode = "down";
      updateLogStatus();
      statsPoll.start();
      pmPoll.start();
      connectLogWs();
    });
  };
})();
