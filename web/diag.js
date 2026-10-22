(function () {
  var logSeq = 0;
  var logPaused = false;
  var logPollTimer = null;
  var logLines = [];
  var logMissed = false;

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
        "<td>" + fmtEpoch(r.bootTime) + "</td>" +
        "<td>" + (r.reason || "—") + "</td>" +
        "<td><span class=\"" + badgeClass(r.class) + "\">" + (r.class || "?") + "</span></td>" +
        "<td>" + (r.cause || "—") + "</td>" +
        "<td>" + fmtUptime(r.prevUptimeS) + "</td>" +
        "<td>" + ((r.activity || []).join(", ") || "—") + "</td>" +
        "<td>" + (r.fwGit || r.fwVersion || "—") + "</td>" +
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
            "<p>Attempts: " + (life.upAttempts || 0) + " lifetime / " + (sess.upAttempts || 0) + " session</p>" +
            "<p>OK: " + (life.upOk || 0) + " / " + (sess.upOk || 0) + " · success " +
            rate(life.upOk, life.upAttempts) + " / " + rate(sess.upOk, sess.upAttempts) + "</p>" +
            "<p>Skipped (net): " + (life.upSkippedNet || 0) + " / " + (sess.upSkippedNet || 0) + "</p>" +
            "<p>Capture fail: " + (life.upCaptureFail || 0) + " / " + (sess.upCaptureFail || 0) +
            " · OOM: " + (life.upOom || 0) + " / " + (sess.upOom || 0) + "</p>" +
            "<p>HTTP 4xx/5xx/other: " + (life.upHttp4xx || 0) + "/" + (sess.upHttp4xx || 0) + " · " +
            (life.upHttp5xx || 0) + "/" + (sess.upHttp5xx || 0) + " · " +
            (life.upHttpOther || 0) + "/" + (sess.upHttpOther || 0) + "</p>" +
            "<p>No connection: " + (life.upNoConn || 0) + " / " + (sess.upNoConn || 0) + "</p>" +
            "<p>Fail streak: " + (res.upStreak || 0) + " (longest " + (res.upLongestStreak || 0) + ")</p>" +
            "<p>Last OK: " + fmtEpoch(res.lastUpOkEpoch) + "</p>" +
            "<p>Last fail: " + fmtEpoch(res.lastUpFailEpoch) + " — " + (res.lastUpFailText || "—") + "</p>" +
            "<p class=\"hint\">Status: " + (res.uploadStatus || "—") + "</p>";
        }
        var eco = document.getElementById("diagEcowitt");
        if (eco) {
          eco.innerHTML =
            "<p>Attempts: " + (life.ecoAttempts || 0) + " / " + (sess.ecoAttempts || 0) + "</p>" +
            "<p>OK: " + (life.ecoOk || 0) + " / " + (sess.ecoOk || 0) + "</p>" +
            "<p>Fail: " + (life.ecoFail || 0) + " / " + (sess.ecoFail || 0) + "</p>" +
            "<p class=\"hint\">" + (res.ecowittStatus || "—") + "</p>";
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
              "<td>" + (f.epoch ? fmtEpoch(f.epoch) : fmtUptime(f.uptimeS)) + "</td>" +
              "<td>" + (f.kind || "") + "</td>" +
              "<td>" + (f.detail || "") + "</td>" +
              "<td>" + (f.code !== undefined && f.code >= 0 ? f.code : "—") + "</td>";
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
      var cd = r.coredump;
      var cdText = typeof cd === "string" ? cd : JSON.stringify(cd, null, 2);
      var hint = cd && cd.decodeHint ? cd.decodeHint : "";
      box.innerHTML =
        "<h3>Report detail</h3>" +
        "<pre class=\"mono\">Heap free " + (r.heapFree || "—") + " min " + (r.heapMin || "—") +
        " · PSRAM " + (r.psramFree || "—") + " · RSSI " + (r.rssi || "—") + "</pre>" +
        "<pre class=\"mono log-detail\">" + (r.prevLines || []).join("\n") + "</pre>" +
        "<pre class=\"mono\">" + cdText + "</pre>" +
        (hint ? "<button type=\"button\" class=\"common-btn\" id=\"diagCopyDecode\">Copy decode command</button>" : "");
      box.style.display = "block";
      var btn = document.getElementById("diagCopyDecode");
      if (btn) {
        btn.onclick = function () {
          navigator.clipboard.writeText(hint).catch(function () {});
          toast("Copied", "ok");
        };
      }
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
