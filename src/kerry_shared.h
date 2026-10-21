#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>

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
void kerryCamDiscardFrames(int count);
bool kerryCameraReady();
const char *kerryCameraSensorName();
void kerryImageGet(JsonDocument &doc);
const char *kerryImageSet(int framesize, int brightness, int contrast, int saturation, int flipHSet, int flipVSet);

bool kerryUploadNow(char *message, size_t messageLen);
void kerryRetryPortal();

bool kerryCaptureUploadJpeg(const char *why, uint8_t **jpeg, size_t *len, char *err, size_t errLen);
bool kerryCaptureBlocked(const char **why);
bool kerryTryBeginUpload();
void kerryEndUpload();
bool kerryEcoNetBusy();
bool kerryTryBeginEcoNet();
void kerryEndEcoNet();
void kerryWdtLenient();
void kerryWdtRestore();

void kerryNetworkFillJson(JsonDocument &doc);
const char *kerryWifiScan(JsonDocument &doc);

void webServerRegister(httpd_handle_t server);
