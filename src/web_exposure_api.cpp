#include "web_exposure_api.h"

#include "kerry_cam_autoswitch.h"
#include "kerry_web_util.h"

#include <ArduinoJson.h>

#include <string.h>

static esp_err_t apiExposureAutoGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  doc["ok"] = true;
  kerryCamAutoSwitchFillJson(doc);
  return sendJson(req, doc);
}

static bool readSetting(JsonObjectConst in, const char *key, int *out) {
  if (!in[key].is<int>()) return false;
  *out = in[key].as<int>();
  return true;
}

static esp_err_t apiExposureAutoPost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  JsonDocument out;
  if (deserializeJson(in, readBody(req, 512))) {
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }

  const char *action = in["action"] | "";
  const char *err = nullptr;
  const char *okMessage = "OK";
  if (strcmp(action, "saveNight") == 0) {
    err = kerryCamAutoSwitchSaveNight();
    okMessage = "Night preset saved";
  } else if (strcmp(action, "applyNight") == 0) {
    err = kerryCamAutoSwitchApply(KERRY_CAM_MODE_NIGHT);
    okMessage = "Night settings applied";
  } else if (strcmp(action, "applyDay") == 0) {
    err = kerryCamAutoSwitchApply(KERRY_CAM_MODE_DAY);
    okMessage = "Day settings applied";
  } else if (strcmp(action, "settings") == 0) {
    int high = 0;
    int low = 0;
    int interval = 0;
    int samples = 0;
    if (!in["enabled"].is<bool>() || !readSetting(in.as<JsonObjectConst>(), "highLuma", &high) ||
        !readSetting(in.as<JsonObjectConst>(), "lowLuma", &low) ||
        !readSetting(in.as<JsonObjectConst>(), "intervalMin", &interval) ||
        !readSetting(in.as<JsonObjectConst>(), "samples", &samples)) {
      err = "Missing settings";
    } else if (high < 0 || high > 255 || low < 0 || low > 255) {
      err = "Luma thresholds out of range";
    } else if (interval < 0 || interval > 255) {
      err = "intervalMin out of range";
    } else if (samples < 0 || samples > 255) {
      err = "samples out of range";
    } else {
      kerry_cam_autoswitch_cfg_t cfg;
      cfg.enabled = in["enabled"].as<bool>();
      cfg.highLuma = (uint8_t)high;
      cfg.lowLuma = (uint8_t)low;
      cfg.intervalMin = (uint8_t)interval;
      cfg.samples = (uint8_t)samples;
      err = kerryCamAutoSwitchSetCfg(&cfg);
      okMessage = "Switching settings saved";
    }
  } else {
    err = "Unknown action";
  }

  out["ok"] = err == nullptr;
  out["message"] = err ? err : okMessage;
  if (!err) kerryCamAutoSwitchFillJson(out);
  return sendJson(req, out);
}

void kerryWebExposureApiRegister(httpd_handle_t server) {
  httpd_uri_t getUri = {
      .uri = "/api/exposure/auto", .method = HTTP_GET, .handler = apiExposureAutoGet, .user_ctx = nullptr};
  httpd_uri_t postUri = {
      .uri = "/api/exposure/auto", .method = HTTP_POST, .handler = apiExposureAutoPost, .user_ctx = nullptr};
  registerUri(server, &getUri);
  registerUri(server, &postUri);
}
