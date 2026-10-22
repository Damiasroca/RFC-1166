#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_camera.h>
#include <esp_http_server.h>

#include "kerry_cam_ov2640.h"

extern const char *AP_SSID;
extern const char *AP_PASS;
extern const IPAddress AP_IP;

extern String wifiSsid;
extern String wifiPass;
extern String portalUser;
extern String portalPass;
extern bool termsAccepted;
extern bool usePortal;
extern uint32_t uploadIntervalSec;
extern char uploadStatus[96];
extern bool portalOk;
extern bool isOV5640;
extern bool isOV3660;

void kerryCopyStatus(char *dest, size_t len);
void kerrySaveConfig();
void kerrySaveUploadSettings();

bool kerryStreamActive();
bool kerryCamGrabActive();
int kerryCamDiscardFrames(int count, uint32_t *tsMs = nullptr, int tsCap = 0);
bool kerryCameraReady();
const char *kerryCameraSensorName();
#define KERRY_SELECTABLE_SIZES_MAX 9
void kerryImageGet(JsonDocument &doc);
int kerryImageSelectableSizes(framesize_t *out, int cap);
const char *kerryImageResolveFramesize(int framesize, framesize_t *requested, framesize_t *effective);
const char *kerryImageSet(int framesize, int brightness, int contrast, int saturation, int flipHSet, int flipVSet);
const char *kerryCamReapplyExposure();

typedef struct {
  uint16_t pid;
  bool haveOv2640Live;
  kerry_ov2640_live_t ov2640;
  bool haveHwLuma;
  uint8_t hwLuma;
  int quality;
} kerry_cam_sensor_snapshot_t;

bool kerryCamReadSensorSnapshot(kerry_cam_sensor_snapshot_t *out);

bool kerryUploadNow(char *message, size_t messageLen);
void kerryRetryPortal();

bool kerryCaptureUploadJpeg(const char *why, uint8_t **jpeg, size_t *len, char *err, size_t errLen);
bool kerryCaptureBlocked(const char **why);
bool kerryTryBeginUpload();
void kerryEndUpload();
bool kerryUploadInProgress();
bool kerryEcoNetBusy();
bool kerryTryBeginEcoNet();
void kerryEndEcoNet();
void kerryWdtLenient();
void kerryWdtRestore();

void kerryNetworkFillJson(JsonDocument &doc);
const char *kerryWifiScan(JsonDocument &doc);

void webServerRegister(httpd_handle_t server);
