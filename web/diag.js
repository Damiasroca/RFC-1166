(function () {
  var logSeq = 0;
  var logPaused = false;
  var logPollTimer = null;
  var logLines = [];
  var logMissed = false;
  var reports = [];
  var openReportIdx = -1;
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
    list.forEach(function (r, idx) {
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
      btn.dataset.rpt = String(idx);
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

  function paintReportDetail(r) {
    var box = document.getElementById("diagReportDetail");
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
    box.appendChild(el("h3", "tile-group", "Last log lines"));
    var lines = r.prevLines || [];
    if (lines.length) {
      box.appendChild(el("pre", "mono-block", lines.join("\n")));
    } else {
      box.appendChild(el("p", "hint", "No log lines (power loss clears them)"));
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

  function paintReboots(data) {
    var ctr = data.counters || {};
    var subBoot = "Current boot #" + fmtInt(data.bootSeq || 0) + ", up " + fmtUptime(data.uptimeS);
    setTile("pmSummary", "recorded", fmtInt(ctr.total || 0), subBoot);
    setTile("pmSummary", "crashes", fmtInt(ctr.crash || 0), "", (ctr.crash || 0) > 0 ? "err" : undefined);
    setTile("pmSummary", "deliberate", fmtInt(ctr.deliberate || 0), "Watchdog, user, settings, OTA");
    setTile("pmSummary", "power", fmtInt(ctr.power || 0), "Power-on or brownout", (ctr.power || 0) > 0 ? "warn" : undefined);
    setTile("pmSummary", "thisBoot", data.reason || "—", data.class || "—");
    paintReasonTiles(ctr.byReason || {});
    reports = data.reports || [];
    paintReportsTable(reports);
    if (openReportIdx >= 0 && openReportIdx < reports.length) paintReportDetail(reports[openReportIdx]);
    else {
      openReportIdx = -1;
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

  function loadPostmortem() {
    ajax({
      url: "/api/postmortem",
      success: function (res) {
        paintReboots(res);
      }
    });
  }

  function loadStats() {
    ajax({
      url: "/api/stats",
      success: function (res) {
        paintStats(res);
      }
    });
  }

  function updateLogStatus() {
    var st = document.getElementById("diagLogStatus");
    if (!st) return;
    if (logPaused) st.textContent = "Paused";
    else if (logMissed) st.textContent = "Lines were missed";
    else st.textContent = "Live · updated every 5 s";
  }

  function renderLogPanel() {
    var pre = document.getElementById("diagLogPanel");
    if (!pre) return;
    var atBottom = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 12;
    var text = logLines.join("\n");
    if (logMissed && text) text = "— lines missed —\n" + text;
    pre.textContent = text;
    if (atBottom) pre.scrollTop = pre.scrollHeight;
    updateLogStatus();
  }

  function pollLog() {
    if (logPaused) return;
    ajax({
      url: "/api/log?since=" + logSeq,
      success: function (res) {
        if (res.truncated) logMissed = true;
        (res.lines || []).forEach(function (line) {
          logLines.push(line);
          while (logLines.length > 2000) logLines.shift();
        });
        if (res.lastSeq) logSeq = res.lastSeq;
        renderLogPanel();
      }
    });
  }

  function startLogPoll() {
    if (logPollTimer) clearInterval(logPollTimer);
    logPollTimer = setInterval(pollLog, 5000);
    pollLog();
  }

  function stopLogPoll() {
    if (logPollTimer) clearInterval(logPollTimer);
    logPollTimer = null;
  }

  document.addEventListener("visibilitychange", function () {
    if (document.hidden) stopLogPoll();
    else startLogPoll();
  });

  document.addEventListener("click", function (ev) {
    var t = ev.target;
    if (!t || !t.dataset || t.dataset.rpt === undefined) return;
    var idx = parseInt(t.dataset.rpt, 10);
    if (isNaN(idx) || !reports[idx]) return;
    if (openReportIdx === idx) {
      openReportIdx = -1;
      paintReportDetail(null);
    } else {
      openReportIdx = idx;
      paintReportDetail(reports[idx]);
    }
  });

  window.diagClearHistory = function () {
    if (!confirm("Clear reboot history and counters?")) return;
    ajax({
      method: "POST",
      url: "/api/postmortem",
      data: { action: "clear" },
      success: function () {
        openReportIdx = -1;
        loadPostmortem();
        toast("Cleared", "ok");
      }
    });
  };

  window.diagDownloadReports = function () {
    ajax({
      url: "/api/postmortem",
      success: function (res) {
        var blob = new Blob([JSON.stringify(res, null, 2)], { type: "text/plain" });
        var a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = "kerry-reboot-reports.json";
        a.click();
      }
    });
  };

  window.diagResetStats = function () {
    if (!confirm("Reset upload statistics?")) return;
    ajax({
      method: "POST",
      url: "/api/stats",
      data: { action: "reset" },
      success: function () {
        loadStats();
        toast("Counters reset", "ok");
      }
    });
  };

  window.diagToggleLogPause = function () {
    logPaused = !logPaused;
    var btn = document.getElementById("diagLogPauseBtn");
    if (btn) btn.textContent = logPaused ? "Resume" : "Pause";
    updateLogStatus();
    if (!logPaused) pollLog();
  };

  window.diagClearLogView = function () {
    logLines = [];
    logMissed = false;
    renderLogPanel();
  };

  window.diagDownloadLog = function () {
    var blob = new Blob([logLines.join("\n")], { type: "text/plain" });
    var a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "kerry-log.txt";
    a.click();
  };

  window.diagInit = function () {
    initTileLayouts();
    loadPostmortem();
    loadStats();
    startLogPoll();
    setInterval(loadStats, 15000);
    setInterval(loadPostmortem, 30000);
  };
})();
