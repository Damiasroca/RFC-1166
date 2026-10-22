(function () {
  var logSeq = 0;
  var logPaused = false;
  var logPollTimer = null;
  var logLines = [];
  var logMissed = false;

  function esc(s) {
    if (s === null || s === undefined) return "";
    s = String(s);
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#39;");
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
    if (d > 0) return d + "d " + h + "h " + m + "m";
    if (h > 0) return h + "h " + m + "m " + s + "s";
    if (m > 0) return m + "m " + s + "s";
    return s + "s";
  }

  function fmtEpoch(ep) {
    if (!ep) return "—";
    try {
      return new Date(ep * 1000).toLocaleString();
    } catch (e) {
      return String(ep);
    }
  }

  function badgeClass(cls) {
    if (cls === "crash") return "badge err";
    if (cls === "deliberate") return "badge ok";
    if (cls === "power") return "badge";
    return "badge muted";
  }

  function paintReboots(data) {
    var ctr = data.counters || {};
    var el = document.getElementById("diagRebootSummary");
    if (!el) return;
    var parts = ["Boots: " + (ctr.total || 0)];
    parts.push("deliberate " + (ctr.deliberate || 0));
    parts.push("crash " + (ctr.crash || 0));
    parts.push("power " + (ctr.power || 0));
    if (data.storage) parts.push("storage " + data.storage);
    el.textContent = parts.join(" · ");
    var by = ctr.byReason || {};
    var reasons = document.getElementById("diagReasonList");
    if (reasons) {
      reasons.innerHTML = "";
      Object.keys(by).sort().forEach(function (k) {
        var li = document.createElement("li");
        li.textContent = k + ": " + by[k];
        reasons.appendChild(li);
      });
    }
    var tbody = document.getElementById("diagReportsBody");
    if (!tbody) return;
    tbody.innerHTML = "";
    var reports = data.reports || [];
    reports.forEach(function (r, idx) {
      var tr = document.createElement("tr");
      tr.innerHTML =
        "<td>" + esc(fmtEpoch(r.bootTime)) + "</td>" +
        "<td>" + esc(r.reason || "—") + "</td>" +
        "<td><span class=\"" + badgeClass(r.class) + "\">" + esc(r.class || "?") + "</span></td>" +
        "<td>" + esc(r.cause || "—") + "</td>" +
        "<td>" + esc(fmtUptime(r.prevUptimeS)) + "</td>" +
        "<td>" + esc((r.activity || []).join(", ") || "—") + "</td>" +
        "<td>" + esc(r.fwGit || r.fwVersion || "—") + "</td>" +
        "<td><button type=\"button\" class=\"linkish\" data-rpt=\"" + idx + "\">Details</button></td>";
      tbody.appendChild(tr);
    });
    window._diagReports = reports;
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
        var life = res.lifetime || {};
        var sess = res.session || {};
        function rate(ok, att) {
          if (!att) return "—";
          return Math.round((ok / att) * 100) + "%";
        }
        var finestra = document.getElementById("diagFinestra");
        if (finestra) {
          finestra.innerHTML =
            "<p>Attempts: " + esc(life.upAttempts || 0) + " lifetime / " + esc(sess.upAttempts || 0) + " session</p>" +
            "<p>OK: " + esc(life.upOk || 0) + " / " + esc(sess.upOk || 0) + " · success " +
            esc(rate(life.upOk, life.upAttempts)) + " / " + esc(rate(sess.upOk, sess.upAttempts)) + "</p>" +
            "<p>Skipped (net): " + esc(life.upSkippedNet || 0) + " / " + esc(sess.upSkippedNet || 0) + "</p>" +
            "<p>Capture fail: " + esc(life.upCaptureFail || 0) + " / " + esc(sess.upCaptureFail || 0) +
            " · OOM: " + esc(life.upOom || 0) + " / " + esc(sess.upOom || 0) + "</p>" +
            "<p>HTTP 4xx/5xx/other: " + esc(life.upHttp4xx || 0) + "/" + esc(sess.upHttp4xx || 0) + " · " +
            esc(life.upHttp5xx || 0) + "/" + esc(sess.upHttp5xx || 0) + " · " +
            esc(life.upHttpOther || 0) + "/" + esc(sess.upHttpOther || 0) + "</p>" +
            "<p>No connection: " + esc(life.upNoConn || 0) + " / " + esc(sess.upNoConn || 0) + "</p>" +
            "<p>Fail streak: " + esc(res.upStreak || 0) + " (longest " + esc(res.upLongestStreak || 0) + ")</p>" +
            "<p>Last OK: " + esc(fmtEpoch(res.lastUpOkEpoch)) + "</p>" +
            "<p>Last fail: " + esc(fmtEpoch(res.lastUpFailEpoch)) + " — " + esc(res.lastUpFailText || "—") + "</p>" +
            "<p class=\"hint\">Status: " + esc(res.uploadStatus || "—") + "</p>";
        }
        var eco = document.getElementById("diagEcowitt");
        if (eco) {
          eco.innerHTML =
            "<p>Attempts: " + esc(life.ecoAttempts || 0) + " / " + esc(sess.ecoAttempts || 0) + "</p>" +
            "<p>OK: " + esc(life.ecoOk || 0) + " / " + esc(sess.ecoOk || 0) + "</p>" +
            "<p>Fail: " + esc(life.ecoFail || 0) + " / " + esc(sess.ecoFail || 0) + "</p>" +
            "<p class=\"hint\">" + esc(res.ecowittStatus || "—") + "</p>";
        }
        var misc = document.getElementById("diagMisc");
        if (misc) {
          misc.textContent =
            "Camera fail " + (life.camFail || 0) + "/" + (sess.camFail || 0) +
            " · reinit " + (life.camReinit || 0) + "/" + (sess.camReinit || 0) +
            " · WD reboot " + (life.wdReboots || 0) + "/" + (sess.wdReboots || 0) +
            " · WD held " + (life.wdHeld || 0) + "/" + (sess.wdHeld || 0);
        }
        var ftbody = document.getElementById("diagFailBody");
        if (ftbody) {
          ftbody.innerHTML = "";
          (res.recentFailures || []).slice().reverse().forEach(function (f) {
            var tr = document.createElement("tr");
            tr.innerHTML =
              "<td>" + esc(f.epoch ? fmtEpoch(f.epoch) : fmtUptime(f.uptimeS)) + "</td>" +
              "<td>" + esc(f.kind || "") + "</td>" +
              "<td>" + esc(f.detail || "") + "</td>" +
              "<td>" + esc(f.code !== undefined && f.code >= 0 ? f.code : "—") + "</td>";
            ftbody.appendChild(tr);
          });
        }
      }
    });
  }

  function renderLogPanel() {
    var pre = document.getElementById("diagLogPanel");
    if (!pre) return;
    var text = logLines.join("\n");
    if (logMissed) text = "— lines missed —\n" + text;
    pre.textContent = text;
    pre.scrollTop = pre.scrollHeight;
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
    if (t && t.dataset && t.dataset.rpt !== undefined) {
      var r = (window._diagReports || [])[parseInt(t.dataset.rpt, 10)];
      var box = document.getElementById("diagReportDetail");
      if (!box || !r) return;
      box.innerHTML = "";
      var h3 = document.createElement("h3");
      h3.textContent = "Report detail";
      box.appendChild(h3);
      var heap = document.createElement("pre");
      heap.className = "mono";
      heap.textContent =
        "Heap free " + (r.heapFree != null ? r.heapFree : "—") +
        " min " + (r.heapMin != null ? r.heapMin : "—") +
        " · PSRAM " + (r.psramFree != null ? r.psramFree : "—") +
        " · RSSI " + (r.rssi != null ? r.rssi : "—");
      box.appendChild(heap);
      var linesPre = document.createElement("pre");
      linesPre.className = "mono log-detail";
      linesPre.textContent = (r.prevLines || []).join("\n");
      box.appendChild(linesPre);
      var cd = r.coredump;
      var cdPre = document.createElement("pre");
      cdPre.className = "mono";
      if (typeof cd === "string") cdPre.textContent = cd;
      else cdPre.textContent = JSON.stringify(cd, null, 2);
      box.appendChild(cdPre);
      var hint = cd && typeof cd === "object" && cd.decodeHint ? cd.decodeHint : "";
      if (hint) {
        var btn = document.createElement("button");
        btn.type = "button";
        btn.className = "common-btn";
        btn.textContent = "Copy decode command";
        btn.onclick = function () {
          navigator.clipboard.writeText(hint).catch(function () {});
          toast("Copied", "ok");
        };
        box.appendChild(btn);
      }
      box.style.display = "block";
    }
  });

  window.diagClearHistory = function () {
    if (!confirm("Clear reboot history and counters?")) return;
    ajax({
      method: "POST",
      url: "/api/postmortem",
      data: { action: "clear" },
      success: function () {
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
    loadPostmortem();
    loadStats();
    startLogPoll();
    setInterval(loadStats, 15000);
    setInterval(loadPostmortem, 30000);
  };
})();
