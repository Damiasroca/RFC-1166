#include "web_exposure_api.h"

#include "kerry_cam_autoswitch.h"
#include "kerry_web_util.h"

#include <ArduinoJson.h>

#include <math.h>
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

static bool readEv(JsonObjectConst in, const char *key, float *out) {
  if (in[key].is<float>()) {
    *out = in[key].as<float>();
    return true;
  }
  if (in[key].is<int>()) {
    *out = (float)in[key].as<int>();
    return true;
  }
  return false;
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
    err = kerryCamAutoSwitchApply(KERRY_CAM_MODE_NIGHT, false, NAN);
    okMessage = "Night settings applied";
  } else if (strcmp(action, "applyDay") == 0) {
    err = kerryCamAutoSwitchApply(KERRY_CAM_MODE_DAY, false, NAN);
    okMessage = "Day settings applied";
  } else if (strcmp(action, "settings") == 0) {
    float lowEv = 0;
    float highEv = 0;
    int interval = 0;
    int samples = 0;
    int dwell = 0;
    JsonObjectConst obj = in.as<JsonObjectConst>();
    if (!in["enabled"].is<bool>() || !readEv(obj, "lowEv", &lowEv) || !readEv(obj, "highEv", &highEv) ||
        !readSetting(obj, "intervalMin", &interval) || !readSetting(obj, "samples", &samples) ||
        !readSetting(obj, "minDwellMin", &dwell)) {
      err = "Missing settings";
    } else {
      long lowT = lroundf(lowEv * 10.0f);
      long highT = lroundf(highEv * 10.0f);
      if (lowT < -200 || lowT > 200 || highT < -200 || highT > 200 || lowT + 10 > highT) {
        err = "EV thresholds out of range";
      } else if (interval < 0 || interval > 255) {
        err = "intervalMin out of range";
      } else if (samples < 0 || samples > 255) {
        err = "samples out of range";
      } else if (dwell < 0 || dwell > 255) {
        err = "minDwellMin out of range";
      } else {
        kerry_cam_autoswitch_cfg_t cfg;
        cfg.enabled = in["enabled"].as<bool>();
        cfg.lowEvTenths = (int16_t)lowT;
        cfg.highEvTenths = (int16_t)highT;
        cfg.intervalMin = (uint8_t)interval;
        cfg.samples = (uint8_t)samples;
        cfg.minDwellMin = (uint8_t)dwell;
        err = kerryCamAutoSwitchSetCfg(&cfg);
        okMessage = "Switching settings saved";
      }
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
