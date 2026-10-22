function autoSceneText(scene) {
  if (!scene) return "no sample yet";
  if (scene.status === "ok" && scene.ev != null) return "EV " + Number(scene.ev).toFixed(1);
  if (scene.status === "clipped_high") return "clipped high";
  if (scene.status === "clipped_low") return "clipped low";
  return "unavailable";
}

function autoSwitchText(sw) {
  if (!sw) return "no switch yet";
  var text = sw.ok ? "switched to " + (sw.to || "?") : "switch to " + (sw.to || "?") + " failed";
  text += " (" + (sw.trigger || "?") + ")";
  if (sw.ev != null) text += " at EV " + Number(sw.ev).toFixed(1);
  if (sw.message) text += ": " + sw.message;
  if (sw.ageS != null) text += " · " + sw.ageS + "s ago";
  return text;
}

function autoStatusText(res) {
  if (res.available === false) return "Automatic switching needs an OV2640 sensor";
  var mode = res.mode || "unknown";
  var luma = res.lastLuma ? "luma " + res.lastLuma.avg : "no luma";
  if (res.lastSampleAgeS != null) luma += " · " + res.lastSampleAgeS + "s ago";
  var samples = res.samples != null ? res.samples : 0;
  var preset = res.hasNight ? "night preset saved" : "no night preset";
  return "Mode: " + mode + " · " + autoSceneText(res.lastScene) + " · " + luma +
    " · low " + (res.lowCount || 0) + "/" + samples +
    " high " + (res.highCount || 0) + "/" + samples +
    " · " + autoSwitchText(res.lastSwitch) + " · " + preset;
}

function setAutoControls(enabled) {
  var ids = ["autoEnabled", "autoLow", "autoHigh", "autoInterval", "autoSamples", "autoDwell", "saveAutoBtn"];
  for (var i = 0; i < ids.length; i++) {
    var el = document.getElementById(ids[i]);
    if (el) el.disabled = !enabled;
  }
}

function applyAutoForm(res) {
  var available = res.available !== false;
  setAutoControls(available);
  document.getElementById("autoEnabled").checked = !!res.enabled;
  if (res.lowEv != null) document.getElementById("autoLow").value = res.lowEv;
  if (res.highEv != null) document.getElementById("autoHigh").value = res.highEv;
  if (res.intervalMin != null) document.getElementById("autoInterval").value = res.intervalMin;
  if (res.samples != null) document.getElementById("autoSamples").value = res.samples;
  if (res.minDwellMin != null) document.getElementById("autoDwell").value = res.minDwellMin;
  document.getElementById("autoStatus").textContent = autoStatusText(res);
}

function loadAutoStatus() {
  ajax({
    url: "/api/exposure/auto",
    success: function (res) {
      if (res.ok) applyAutoForm(res);
    }
  });
}

function postAuto(body, reloadExposure) {
  ajax({
    method: "POST",
    url: "/api/exposure/auto",
    data: body,
    success: function (res) {
      toast(res.message || (res.ok ? "OK" : "Failed"), res.ok ? "ok" : "err");
      if (res.ok) {
        applyAutoForm(res);
        if (reloadExposure) loadImageSettings();
      }
    },
    error: function (_, res) {
      toast((res && res.message) || "Request failed", "err");
    }
  });
}

document.getElementById("saveNightBtn").onclick = function () {
  postAuto({ action: "saveNight" }, false);
};

document.getElementById("applyNightBtn").onclick = function () {
  postAuto({ action: "applyNight" }, true);
};

document.getElementById("applyDayBtn").onclick = function () {
  postAuto({ action: "applyDay" }, true);
};

document.getElementById("saveAutoBtn").onclick = function () {
  postAuto({
    action: "settings",
    enabled: document.getElementById("autoEnabled").checked,
    lowEv: parseFloat(document.getElementById("autoLow").value),
    highEv: parseFloat(document.getElementById("autoHigh").value),
    intervalMin: parseInt(document.getElementById("autoInterval").value, 10),
    samples: parseInt(document.getElementById("autoSamples").value, 10),
    minDwellMin: parseInt(document.getElementById("autoDwell").value, 10)
  }, false);
};
