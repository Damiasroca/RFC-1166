function autoStatusText(res) {
  var mode = res.mode || "unknown";
  var luma = "no sample yet";
  if (res.lastLuma) {
    luma = "luma " + res.lastLuma.avg + "/" + res.lastLuma.min + "/" + res.lastLuma.max;
    if (res.lastSampleAgeS != null) luma += " · " + res.lastSampleAgeS + "s ago";
  }
  var samples = res.samples != null ? res.samples : 0;
  var preset = res.hasNight ? "night preset saved" : "no night preset";
  return "Mode: " + mode + " · " + luma + " · low " + (res.lowCount || 0) + "/" + samples +
    " high " + (res.highCount || 0) + "/" + samples + " · " + preset;
}

function applyAutoForm(res) {
  document.getElementById("autoEnabled").checked = !!res.enabled;
  if (res.highLuma != null) document.getElementById("autoHigh").value = res.highLuma;
  if (res.lowLuma != null) document.getElementById("autoLow").value = res.lowLuma;
  if (res.intervalMin != null) document.getElementById("autoInterval").value = res.intervalMin;
  if (res.samples != null) document.getElementById("autoSamples").value = res.samples;
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
    highLuma: parseInt(document.getElementById("autoHigh").value, 10),
    lowLuma: parseInt(document.getElementById("autoLow").value, 10),
    intervalMin: parseInt(document.getElementById("autoInterval").value, 10),
    samples: parseInt(document.getElementById("autoSamples").value, 10)
  }, false);
};
