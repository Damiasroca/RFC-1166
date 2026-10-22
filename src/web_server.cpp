#include "ecowitt.h"
#include "geopulse.h"
#include "kerry_awb.h"
#include "kerry_cam_exposure.h"
#include "kerry_health.h"
#include "kerry_log.h"
#include "kerry_shared.h"
#include "kerry_version.h"
#include "kerry_time.h"
#include "kerry_web_util.h"
#include "overlay/overlay_cfg.h"
#include "web_assets.h"
#include "web_exposure_api.h"

#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <ArduinoJson.h>
#include <WiFi.h>

static bool gLoggedIn = false;

bool cookieAuth(httpd_req_t *req) {
  if (gLoggedIn) return true;
  char cookie[128];
  if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
  return strstr(cookie, "KerryCamAuth=1") != nullptr;
}

static void setAuthCookie(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Set-Cookie", "KerryCamAuth=1; Path=/; HttpOnly");
}

static void clearAuthCookie(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Set-Cookie", "KerryCamAuth=0; Path=/; Max-Age=0; HttpOnly");
}

static esp_err_t sendAsset(httpd_req_t *req, const WebAsset *asset) {
  httpd_resp_set_type(req, asset->content_type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, (const char *)asset->data, asset->size);
}

static esp_err_t rootHandler(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", cookieAuth(req) ? "/index.html" : "/login.html");
  return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t staticHandler(httpd_req_t *req) {
  const WebAsset *asset = webFindAsset(req->uri);
  if (!asset) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
  if (strcmp(req->uri, "/login.html") != 0 && strcmp(req->uri, "/app.css") != 0 &&
      strcmp(req->uri, "/app.js") != 0) {
    if (!requireAuth(req, false)) return ESP_OK;
  }
  return sendAsset(req, asset);
}

static esp_err_t apiVersion(httpd_req_t *req) {
  JsonDocument doc;
  const esp_app_desc_t *app = esp_app_get_description();
  doc["version"] = kerryFirmwareVersion();
  doc["buildId"] = kerryFirmwareBuildId();
  doc["buildEnv"] = kerryFirmwareEnv();
  doc["git"] = kerryFirmwareGit();
  doc["label"] = kerryFirmwareLabel();
  doc["idf"] = app ? app->idf_ver : "";
  doc["date"] = app ? app->date : "";
  doc["time"] = app ? app->time : "";
  return sendJson(req, doc);
}

static esp_err_t apiAuth(httpd_req_t *req) {
  JsonDocument doc;
  doc["ok"] = cookieAuth(req);
  return sendJson(req, doc);
}

static esp_err_t apiLogin(httpd_req_t *req) {
  JsonDocument in;
  if (deserializeJson(in, readBody(req, 256))) {
    JsonDocument out;
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }
  const char *pwd = in["password"] | "";
  bool ok = strcmp(pwd, AP_PASS) == 0;
  JsonDocument out;
  if (ok) {
    gLoggedIn = true;
    setAuthCookie(req);
    out["ok"] = true;
  } else {
    gLoggedIn = false;
    out["ok"] = false;
    out["message"] = "Wrong password";
  }
  return sendJson(req, out);
}

static esp_err_t apiLogout(httpd_req_t *req) {
  gLoggedIn = false;
  clearAuthCookie(req);
  JsonDocument doc;
  doc["ok"] = true;
  return sendJson(req, doc);
}

static esp_err_t apiUploadNow(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument out;
  char msg[96];
  bool ok = kerryUploadNow(msg, sizeof msg);
  out["ok"] = ok;
  out["message"] = msg;
  out["uploadStatus"] = uploadStatus;
  return sendJson(req, out);
}

static esp_err_t apiPortalRetry(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  kerryRetryPortal();
  JsonDocument out;
  out["ok"] = true;
  out["message"] = "Portal retry scheduled";
  return sendJson(req, out);
}

static esp_err_t apiStatus(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  char status[160];
  kerryCopyStatus(status, sizeof(status));
  JsonDocument doc;
  JsonDocument netDoc;
  kerryNetworkFillJson(netDoc);
  doc["wifiState"] = netDoc["wifiIp"] | "Not configured";
  if (strcmp(doc["wifiState"].as<const char *>(), "—") == 0) {
    doc["wifiState"] = netDoc["wifiStatus"] | "Joining router";
  }
  doc["portalState"] = portalOk ? "Open" : "Closed";
  doc["apState"] = String(AP_SSID) + " @ " + AP_IP.toString();
  doc["uploadState"] = uploadStatus;
  doc["ecowittState"] = ecowittSummary();
  doc["cameraReady"] = kerryCameraReady();
  doc["sensor"] = kerryCameraSensorName();
  doc["statusText"] = status;
  JsonDocument healthDoc;
  JsonDocument timeDoc;
  kerryHealthFillStatus(healthDoc);
  kerryTimeFillStatus(timeDoc);
  doc["health"].set(healthDoc);
  doc["time"].set(timeDoc);
  return sendJson(req, doc);
}

static esp_err_t apiOverlayGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  kerryGeoPulseFillConfig(doc);
  return sendJson(req, doc);
}

static esp_err_t apiOverlayPost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  if (deserializeJson(in, readBody(req, 4096))) {
    JsonDocument out;
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }
  const char *err = kerryGeoPulseApplyConfig(in);
  JsonDocument out;
  if (err) {
    out["ok"] = false;
    out["message"] = err;
    return sendJson(req, out);
  }
  out["ok"] = true;
  out["message"] = "Overlay settings saved";
  return sendJson(req, out);
}

static esp_err_t apiOverlayTest(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  String raw = readBody(req, 4096);
  if (raw.length()) {
    JsonDocument in;
    if (!deserializeJson(in, raw)) {
      const char *user = in["gpUser"] | "";
      const char *pass = in["gpPass"] | "";
      const char *device = in["gpDevice"] | "";
      if (user[0] || pass[0] || device[0]) kerryGeoPulseSaveCredentials(user, pass, device);
      if (!in["elements"].isNull() || in["enabled"].is<bool>()) kerryGeoPulseApplyConfig(in);
    }
  }
  JsonDocument out;
  ov_nav_t nav;
  if (kerryGeoPulseFetchNav(&nav)) {
    out["ok"] = true;
    out["message"] = "GeoPulse data fetched";
    out["sample"]["time"] = nav.time_text;
    out["sample"]["lat"] = nav.lat;
    out["sample"]["lon"] = nav.lon;
    out["sample"]["speed"] = nav.speed;
    out["sample"]["course"] = nav.course;
    out["sample"]["dev"] = nav.device_id;
    out["sample"]["mtc"] = nav.match_type;
    out["sample"]["req"] = nav.requested;
    out["fetchMs"] = nav.fetch_ms;
  } else {
    out["ok"] = false;
    const char *detail = kerryGeoPulseLastError();
    out["message"] = detail[0] ? detail : "Fetch failed (check ship Wi-Fi and credentials)";
  }
  return sendJson(req, out);
}

static esp_err_t apiNetworkGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  kerryNetworkFillJson(doc);
  return sendJson(req, doc);
}

static esp_err_t apiWifiScan(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  const char *err = kerryWifiScan(doc);
  JsonDocument out;
  if (err) {
    out["ok"] = false;
    out["message"] = err;
    return sendJson(req, out);
  }
  out["ok"] = true;
  out["networks"] = doc["networks"];
  out["count"] = doc["count"];
  return sendJson(req, out);
}

static esp_err_t apiImageGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  kerryImageGet(doc);
  JsonDocument awbDoc;
  kerryAwbFillJson(awbDoc);
  doc["awb"].set(awbDoc);
  return sendJson(req, doc);
}

static esp_err_t apiAwbGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  kerryAwbFillJson(doc);
  return sendJson(req, doc);
}

static esp_err_t apiAwbPost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  if (deserializeJson(in, readBody(req, 1024))) {
    JsonDocument out;
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }
  JsonDocument out;
  if (in["deletePreset"].is<int>()) {
    const char *err = kerryAwbPresetDelete(in["deletePreset"].as<int>());
    if (err) {
      out["ok"] = false;
      out["message"] = err;
      return sendJson(req, out);
    }
    out["ok"] = true;
    out["message"] = "Preset deleted";
    JsonDocument awbOut;
    kerryAwbFillJson(awbOut);
    out["awb"].set(awbOut);
    return sendJson(req, out);
  }
  if (in["savePreset"].is<bool>() && in["savePreset"].as<bool>()) {
    const char *err = kerryAwbPresetSave(in["name"] | "", in["r"] | 0, in["g"] | 0, in["b"] | 0);
    if (err) {
      out["ok"] = false;
      out["message"] = err;
      return sendJson(req, out);
    }
    out["ok"] = true;
    out["message"] = "Preset saved";
    JsonDocument awbOut;
    kerryAwbFillJson(awbOut);
    out["awb"].set(awbOut);
    return sendJson(req, out);
  }
  if (!in["awb"].is<int>() || !in["r"].is<int>() || !in["g"].is<int>() || !in["b"].is<int>()) {
    out["ok"] = false;
    out["message"] = "Missing AWB fields";
    return sendJson(req, out);
  }
  const char *err = kerryAwbSet(in["awb"].as<int>(), in["r"].as<int>(), in["g"].as<int>(), in["b"].as<int>());
  if (err) {
    out["ok"] = false;
    out["message"] = err;
    return sendJson(req, out);
  }
  out["ok"] = true;
  out["message"] = "White balance applied";
  JsonDocument awbOut;
  kerryAwbFillJson(awbOut);
  out["awb"].set(awbOut);
  return sendJson(req, out);
}

static void fillMemoryJson(JsonObject obj) {
  obj["heap_free"] = (uint32_t)esp_get_free_heap_size();
  obj["heap_min"] = (uint32_t)esp_get_minimum_free_heap_size();
  obj["heap_total"] = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_8BIT);
  obj["heap_largest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  obj["internal_free"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  obj["internal_total"] = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  obj["internal_largest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  obj["internal_min"] = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

  obj["dma_free"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
  obj["dma_largest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

  const bool ps = psramFound();
  obj["has_psram"] = ps;
  if (ps) {
    obj["psram_free"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    obj["psram_total"] = (uint32_t)ESP.getPsramSize();
    obj["psram_largest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    obj["psram_min"] = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
  } else {
    obj["psram_free"] = 0;
    obj["psram_total"] = 0;
    obj["psram_largest"] = 0;
    obj["psram_min"] = 0;
  }

  obj["flash_size"] = (uint32_t)ESP.getFlashChipSize();
  obj["sketch_size"] = (uint32_t)ESP.getSketchSize();
  obj["sketch_free"] = (uint32_t)ESP.getFreeSketchSpace();
  obj["uptime_s"] = (uint32_t)(millis() / 1000UL);
}

static esp_err_t apiSystemGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  const esp_app_desc_t *app = esp_app_get_description();
  doc["version"] = kerryFirmwareVersion();
  doc["buildId"] = kerryFirmwareBuildId();
  doc["buildEnv"] = kerryFirmwareEnv();
  doc["git"] = kerryFirmwareGit();
  doc["label"] = kerryFirmwareLabel();
  doc["buildDate"] = app ? app->date : "";
  doc["buildTime"] = app ? app->time : "";
  kerryHealthFillStatus(doc);
  JsonDocument timeDoc;
  kerryTimeFillStatus(timeDoc);
  doc["time"].set(timeDoc);
  JsonDocument memDoc;
  fillMemoryJson(memDoc.to<JsonObject>());
  doc["mem"].set(memDoc);
  return sendJson(req, doc);
}

static esp_err_t apiSystemPost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  if (deserializeJson(in, readBody(req, 512))) {
    JsonDocument out;
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }
  JsonDocument out;
  if (in["reboot"].is<bool>() && in["reboot"].as<bool>()) {
    out["ok"] = true;
    out["message"] = "Rebooting";
    sendJson(req, out);
    delay(400);
    ESP.restart();
    return ESP_OK;
  }
  if (in["wd_en"].is<bool>()) {
    const char *err = kerryHealthSetWatchdog(in["wd_en"].as<bool>(), in["wd_cam_n"] | 3, in["wd_up_n"] | 3,
                                              in["wd_cap_n"] | 3, in["wd_cap_m"] | 60);
    if (err) {
      out["ok"] = false;
      out["message"] = err;
      return sendJson(req, out);
    }
    out["ok"] = true;
    out["message"] = "Watchdog settings saved";
    return sendJson(req, out);
  }
  out["ok"] = false;
  out["message"] = "Unknown system action";
  return sendJson(req, out);
}

static esp_err_t apiImagePost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  if (deserializeJson(in, readBody(req, 1200))) {
    JsonDocument out;
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }

  JsonDocument out;
  if (!in["framesize"].is<int>() || !in["brightness"].is<int>() || !in["contrast"].is<int>() ||
      !in["saturation"].is<int>()) {
    out["ok"] = false;
    out["message"] = "Missing image fields";
    return sendJson(req, out);
  }

  framesize_t targetFs = FRAMESIZE_INVALID;
  const char *fsErr = kerryImageResolveFramesize(in["framesize"].as<int>(), nullptr, &targetFs);
  if (fsErr) {
    out["ok"] = false;
    out["message"] = fsErr;
    return sendJson(req, out);
  }

  if (!in["exposure"].isNull()) {
    const char *expErr = kerryCamExposureStage(in["exposure"], targetFs);
    if (expErr) {
      out["ok"] = false;
      out["message"] = expErr;
      KERRY_LOGW("exposure", "settings rejected: %s", expErr);
      return sendJson(req, out);
    }
  }

  kerryCamExposureActivateStaged();

  int flipHSet = in["flipH"].isNull() ? -1 : (in["flipH"].as<bool>() ? 1 : 0);
  int flipVSet = in["flipV"].isNull() ? -1 : (in["flipV"].as<bool>() ? 1 : 0);
  const char *err = kerryImageSet(in["framesize"].as<int>(), in["brightness"].as<int>(), in["contrast"].as<int>(),
                                   in["saturation"].as<int>(), flipHSet, flipVSet);
  if (err) {
    bool reapply = kerryCamExposureRollbackStaged();
    if (reapply) {
      const char *reErr = kerryCamReapplyExposure();
      if (reErr) KERRY_LOGW("exposure", "reapply after rollback failed: %s", reErr);
    }
    out["ok"] = false;
    out["message"] = err;
    return sendJson(req, out);
  }

  out["ok"] = true;
  if (!kerryCamExposurePersistStaged()) {
    out["message"] = "Image settings saved; exposure was reverted";
  } else {
    out["message"] = "Image settings saved";
  }
  KERRY_LOGI("exposure", "image settings saved from web UI");
  JsonDocument settings;
  kerryImageGet(settings);
  out["settings"] = settings;
  return sendJson(req, out);
}

static esp_err_t apiNetworkPost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  if (deserializeJson(in, readBody(req, 1200))) {
    JsonDocument out;
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }

  String newSsid = in["ssid"] | "";
  newSsid.trim();
  JsonDocument out;
  if (!newSsid.length() || newSsid.length() > 32) {
    out["ok"] = false;
    out["message"] = "Enter a router SSID (max 32 characters).";
    return sendJson(req, out);
  }

  bool portalEnabled = in["usePortal"] | false;
  String newLogin = in["plogin"] | "";
  newLogin.trim();
  const char *ppass = in["ppass"] | "";
  const char *wpass = in["wpass"] | "";
  bool terms = in["terms"] | false;
  int upint = in["upint"] | (int)uploadIntervalSec;

  if (portalEnabled && newLogin.length() && !terms) {
    out["ok"] = false;
    out["message"] = "Accept the ferry terms before saving a portal login.";
    return sendJson(req, out);
  }
  if (newLogin.length() > 40 || strlen(wpass) > 64 || strlen(ppass) > 64) {
    out["ok"] = false;
    out["message"] = "One of the fields is too long.";
    return sendJson(req, out);
  }

  wifiSsid = newSsid;
  if (wpass[0]) wifiPass = wpass;
  usePortal = portalEnabled;
  if (!portalEnabled) {
    portalUser = "";
    portalPass = "";
    termsAccepted = false;
  } else {
    portalUser = newLogin;
    if (!newLogin.length()) {
      portalPass = "";
      termsAccepted = false;
    } else {
      if (ppass[0]) portalPass = ppass;
      termsAccepted = terms;
    }
  }

  if (upint < 0) upint = 0;
  if (upint > 0 && upint < 5) upint = 5;
  if (upint > 86400) upint = 86400;
  uploadIntervalSec = (uint32_t)upint;

  kerrySaveConfig();
  kerrySaveUploadSettings();

  out["ok"] = true;
  out["message"] = "Saved. The camera is restarting.";
  sendJson(req, out);
  delay(400);
  ESP.restart();
  return ESP_OK;
}

static esp_err_t apiEcowittGet(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument doc;
  ecowittFillStatus(doc);
  doc["finestraStatus"] = uploadStatus;
  doc["finestraIntervalSec"] = uploadIntervalSec;
  return sendJson(req, doc);
}

static esp_err_t apiEcowittPost(httpd_req_t *req) {
  if (!requireAuth(req, true)) return ESP_OK;
  JsonDocument in;
  JsonDocument out;
  if (deserializeJson(in, readBody(req, 1024))) {
    out["ok"] = false;
    out["message"] = "Bad request";
    return sendJson(req, out);
  }
  const char *op = in["op"] | "";
  const char *err = nullptr;
  if (strcmp(op, "login") == 0) {
    err = ecowittQueueLogin(in["account"] | "", in["password"] | "");
  } else if (strcmp(op, "register") == 0) {
    err = ecowittQueueRegister(in["name"] | "");
  } else if (strcmp(op, "logout") == 0) {
    err = ecowittQueueLogout();
  } else if (strcmp(op, "upload") == 0) {
    err = ecowittQueueUpload();
  } else if (strcmp(op, "schedule") == 0) {
    if (in["interval"].isNull()) err = "Choose an interval";
    else err = ecowittSetSchedule(in["enabled"] | false, in["interval"] | 0);
  } else {
    err = "Unknown request";
  }
  out["ok"] = err == nullptr;
  if (err) out["message"] = err;
  else if (strcmp(op, "schedule") == 0) out["message"] = "Ecowitt schedule saved";
  else if (strcmp(op, "upload") == 0) out["message"] = "Ecowitt upload queued";
  else out["message"] = "Started";
  return sendJson(req, out);
}

void webServerRegister(httpd_handle_t server) {
  httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = rootHandler, .user_ctx = nullptr};
  httpd_uri_t loginHtml = {.uri = "/login.html", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t indexHtml = {.uri = "/index.html", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t networkHtml = {.uri = "/network.html", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t imageHtml = {.uri = "/image.html", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t overlayHtml = {.uri = "/overlay.html", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t systemHtml = {.uri = "/system.html", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t uploadsHtml = {.uri = "/uploads.html", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t appCss = {.uri = "/app.css", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t appJs = {.uri = "/app.js", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t exposureAutoJs = {
      .uri = "/exposure_auto.js", .method = HTTP_GET, .handler = staticHandler, .user_ctx = nullptr};
  httpd_uri_t apiVersionUri = {.uri = "/api/version", .method = HTTP_GET, .handler = apiVersion, .user_ctx = nullptr};
  httpd_uri_t apiAuthUri = {.uri = "/api/auth", .method = HTTP_GET, .handler = apiAuth, .user_ctx = nullptr};
  httpd_uri_t apiLoginUri = {.uri = "/api/login", .method = HTTP_POST, .handler = apiLogin, .user_ctx = nullptr};
  httpd_uri_t apiLogoutUri = {.uri = "/api/logout", .method = HTTP_POST, .handler = apiLogout, .user_ctx = nullptr};
  httpd_uri_t apiStatusUri = {.uri = "/api/status", .method = HTTP_GET, .handler = apiStatus, .user_ctx = nullptr};
  httpd_uri_t apiNetworkGetUri = {.uri = "/api/network", .method = HTTP_GET, .handler = apiNetworkGet, .user_ctx = nullptr};
  httpd_uri_t apiNetworkPostUri = {.uri = "/api/network", .method = HTTP_POST, .handler = apiNetworkPost, .user_ctx = nullptr};
  httpd_uri_t apiWifiScanUri = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = apiWifiScan, .user_ctx = nullptr};
  httpd_uri_t apiImageGetUri = {.uri = "/api/image", .method = HTTP_GET, .handler = apiImageGet, .user_ctx = nullptr};
  httpd_uri_t apiImagePostUri = {.uri = "/api/image", .method = HTTP_POST, .handler = apiImagePost, .user_ctx = nullptr};
  httpd_uri_t apiOverlayGetUri = {.uri = "/api/overlay", .method = HTTP_GET, .handler = apiOverlayGet, .user_ctx = nullptr};
  httpd_uri_t apiOverlayPostUri = {.uri = "/api/overlay", .method = HTTP_POST, .handler = apiOverlayPost, .user_ctx = nullptr};
  httpd_uri_t apiOverlayTestUri = {.uri = "/api/overlay/test", .method = HTTP_POST, .handler = apiOverlayTest, .user_ctx = nullptr};
  httpd_uri_t apiUploadNowUri = {.uri = "/api/upload/now", .method = HTTP_POST, .handler = apiUploadNow, .user_ctx = nullptr};
  httpd_uri_t apiPortalRetryUri = {.uri = "/api/portal/retry", .method = HTTP_POST, .handler = apiPortalRetry, .user_ctx = nullptr};
  httpd_uri_t apiAwbGetUri = {.uri = "/api/awb", .method = HTTP_GET, .handler = apiAwbGet, .user_ctx = nullptr};
  httpd_uri_t apiAwbPostUri = {.uri = "/api/awb", .method = HTTP_POST, .handler = apiAwbPost, .user_ctx = nullptr};
  httpd_uri_t apiSystemGetUri = {.uri = "/api/system", .method = HTTP_GET, .handler = apiSystemGet, .user_ctx = nullptr};
  httpd_uri_t apiSystemPostUri = {.uri = "/api/system", .method = HTTP_POST, .handler = apiSystemPost, .user_ctx = nullptr};
  httpd_uri_t apiEcowittGetUri = {.uri = "/api/ecowitt", .method = HTTP_GET, .handler = apiEcowittGet, .user_ctx = nullptr};
  httpd_uri_t apiEcowittPostUri = {.uri = "/api/ecowitt", .method = HTTP_POST, .handler = apiEcowittPost, .user_ctx = nullptr};

  registerUri(server, &root);
  registerUri(server, &loginHtml);
  registerUri(server, &indexHtml);
  registerUri(server, &networkHtml);
  registerUri(server, &imageHtml);
  registerUri(server, &overlayHtml);
  registerUri(server, &systemHtml);
  registerUri(server, &uploadsHtml);
  registerUri(server, &appCss);
  registerUri(server, &appJs);
  registerUri(server, &exposureAutoJs);
  registerUri(server, &apiVersionUri);
  registerUri(server, &apiAuthUri);
  registerUri(server, &apiLoginUri);
  registerUri(server, &apiLogoutUri);
  registerUri(server, &apiStatusUri);
  registerUri(server, &apiNetworkGetUri);
  registerUri(server, &apiNetworkPostUri);
  registerUri(server, &apiWifiScanUri);
  registerUri(server, &apiImageGetUri);
  registerUri(server, &apiImagePostUri);
  registerUri(server, &apiOverlayGetUri);
  registerUri(server, &apiOverlayPostUri);
  registerUri(server, &apiOverlayTestUri);
  registerUri(server, &apiUploadNowUri);
  registerUri(server, &apiPortalRetryUri);
  registerUri(server, &apiAwbGetUri);
  registerUri(server, &apiAwbPostUri);
  registerUri(server, &apiSystemGetUri);
  registerUri(server, &apiSystemPostUri);
  registerUri(server, &apiEcowittGetUri);
  registerUri(server, &apiEcowittPostUri);
  kerryWebExposureApiRegister(server);
}
