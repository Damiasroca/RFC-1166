#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/gpio.h>
#include <esp_camera.h>
#include "ESP32_OV5640_AF.h"
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <esp_private/brownout.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include <cstring>

#include "ecowitt.h"
#include "geopulse.h"
#include "kerry_awb.h"
#include "kerry_cam_exposure.h"
#include "kerry_cam_ov2640.h"
#include "kerry_cam_telemetry.h"
#include "kerry_health.h"
#include "kerry_log.h"
#include "kerry_shared.h"
#include "kerry_time.h"
#include "kerry_version.h"
#include "secrets.h"
#include "overlay/overlay.h"
#include "overlay/overlay_cfg.h"
#include "ws_log.h"

#define logf(...) KERRY_LOGI("main", __VA_ARGS__)
#define logw(...) KERRY_LOGW("main", __VA_ARGS__)
#define logd(...) KERRY_LOGD("main", __VA_ARGS__)
#define ulogf(...) KERRY_LOGI("upload", __VA_ARGS__)
#define ulogw(...) KERRY_LOGW("upload", __VA_ARGS__)
#define ulogd(...) KERRY_LOGD("upload", __VA_ARGS__)

// AI Thinker ESP32-CAM and Seeed XIAO ESP32S3 Sense.
// Supports OV2640, OV3660, and OV5640 (auto-detected).
#if defined(ARDUINO_XIAO_ESP32S3)
// Sense camera FPC. User LED is GPIO21, active-low. No separate camera rail.
static const int LED_GPIO = 21;
static const int LED_ON_LEVEL = LOW;
static const int PWDN_GPIO = -1;
static const int CAM_POWER_GPIO = -1;
static const int CAM_EN_GPIO = -1;
static const int RESET_GPIO = -1;
static const int XCLK_GPIO = 10;
static const int XCLK_FREQ_HZ = 20000000;
static const int SIOD_GPIO = 40;
static const int SIOC_GPIO = 39;
static const int Y9_GPIO = 48;
static const int Y8_GPIO = 11;
static const int Y7_GPIO = 12;
static const int Y6_GPIO = 14;
static const int Y5_GPIO = 16;
static const int Y4_GPIO = 18;
static const int Y3_GPIO = 17;
static const int Y2_GPIO = 15;
static const int VSYNC_GPIO = 38;
static const int HREF_GPIO = 47;
static const int PCLK_GPIO = 13;
#else
// Flash LED is GPIO4 and must be driven, otherwise it floats on and browns the board out.
static const int LED_GPIO = 4;
static const int LED_ON_LEVEL = HIGH;
static const int PWDN_GPIO = 32;
static const int CAM_POWER_GPIO = 2;  // HP10 camera 3.3 V rail, active-low
static const int CAM_EN_GPIO = 12;     // HP10 regulator enable, held high
static const int RESET_GPIO = -1;
static const int XCLK_GPIO = 0;
static const int XCLK_FREQ_HZ = 16000000;
static const int SIOD_GPIO = 26;
static const int SIOC_GPIO = 27;
static const int Y9_GPIO = 35;
static const int Y8_GPIO = 34;
static const int Y7_GPIO = 39;
static const int Y6_GPIO = 36;
static const int Y5_GPIO = 21;
static const int Y4_GPIO = 19;
static const int Y3_GPIO = 18;
static const int Y2_GPIO = 5;
static const int VSYNC_GPIO = 25;
static const int HREF_GPIO = 23;
static const int PCLK_GPIO = 22;
#endif
static char apSsid[16] = "RFC-1166";
const char *AP_SSID = apSsid;
const char *AP_PASS = KERRY_AP_PASS;

// RFC-1166 plus the last four hex digits of the station MAC, so two cameras
// do not advertise the same setup network.
static void setApSsidFromMac() {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return;
  snprintf(apSsid, sizeof apSsid, "RFC-1166%02X%02X", mac[4], mac[5]);
}
const IPAddress AP_IP(192, 168, 4, 1);
static const char *PORTAL_HOST = "internet.stenaline.com";
static const uint32_t FLASH_MAX_MS = 30000;
static const uint32_t PORTAL_CHECK_UNAUTH_MS = 45000;
static const uint32_t PORTAL_CHECK_AUTH_MS = 120000;
static const uint8_t PORTAL_AUTH_FAIL_THRESHOLD = 2;
static const uint8_t PORTAL_FULL_CYCLE_REBOOT = 2;
static const uint8_t WIFI_FULL_JOIN_EVERY = 3;
static const uint32_t WIFI_DHCP_WAIT_MS = 20000;
static const uint32_t WIFI_DOWN_REBOOT_MS = 5UL * 60UL * 1000UL;
static const uint32_t NET_REBOOT_RETRY_MS = 60000;
static const char *UPLOAD_URL = KERRY_UPLOAD_URL;
static const char *LIVE_WANT_URL = KERRY_LIVE_WANT_URL;
static const char *LIVE_FRAME_URL = KERRY_LIVE_FRAME_URL;
static const char *UPLOAD_API_KEY = KERRY_UPLOAD_API_KEY;
static const uint32_t LIVE_MAX_MS = 30000;
static const uint32_t LIVE_POLL_MS = 8000;
static const uint32_t UPLOAD_INTERVAL_MIN_SEC = 5;
static const uint32_t UPLOAD_INTERVAL_MAX_SEC = 86400;

String wifiSsid;
String wifiPass;
String portalUser;
String portalPass;
bool termsAccepted = false;
bool usePortal = false;

static bool cameraReady = false;
static size_t g_activeFbCount = 0;
static camera_grab_mode_t g_activeGrabMode = CAMERA_GRAB_WHEN_EMPTY;
bool isOV5640 = false;
bool isOV3660 = false;
#if defined(ARDUINO_XIAO_ESP32S3)
static framesize_t camFrameSize = FRAMESIZE_UXGA;
#else
static framesize_t camFrameSize = FRAMESIZE_SVGA;
#endif
static bool ov5640SketchActive = false;
// Same sizes as the working XIAO OV5640 snapshot sketch. Buffers are allocated
// at init, so init must be larger than the capture size or the JPEG is cut off.
static const framesize_t OV5640_INIT_FRAMESIZE = FRAMESIZE_QSXGA;
static const framesize_t OV5640_AF_FRAMESIZE = FRAMESIZE_SXGA;
static const uint32_t OV5640_FOCUS_TIMEOUT_MS = 8000;
static OV5640 ov5640Af;
static int8_t camBrightness = 0;
static int8_t camContrast = 0;
static int8_t camSaturation = 0;
static bool flipH = false;
static bool flipV = false;
static bool flashOn = false;
static uint32_t flashOnSince = 0;
static volatile bool streamActive = false;
static volatile bool livePushActive = false;
static volatile bool otaInProgress = false;
static volatile bool uploadInProgress = false;
static volatile bool uploadNowRequested = false;
uint32_t uploadIntervalSec = 0;
static uint32_t nextUploadMs = 0;
char uploadStatus[96] = "Cloud upload is off";
bool portalOk = false;
static bool crewAuthenticated = false;
static uint8_t portalAuthFailCycles = 0;
static uint8_t portalFullCycles = 0;
static uint32_t nextPortalMs = 5000;
static uint32_t nextWifiReconnectMs = 0;
static uint16_t wifiReconnectFails = 0;
static bool wifiStaWasConnected = false;
static bool wifiStaHasIp = false;
static volatile bool wifiManualDisconnect = false;
static uint32_t wifiDownSinceMs = 0;
static uint32_t wifiNoIpSinceMs = 0;
static uint32_t nextNetRebootMs = 0;
static const uint32_t WIFI_RECONNECT_MIN_MS = 3000;
static const uint32_t WIFI_RECONNECT_MAX_MS = 60000;
static char portalStatus[160] = "Starting";
static char sessionId[48] = "";
static char portalBody[3800];
static String redirectPath;
static String probeLocation;
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t camLock = nullptr;
static volatile bool camGrabActive = false;
static volatile bool camLockHeld = false;
static httpd_handle_t httpd = nullptr;
static portMUX_TYPE s_workMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_ecoNet = false;
static volatile bool s_portalNet = false;

bool kerryEcoNetBusy() { return s_ecoNet; }

bool kerryCaptureBlocked(const char **why) {
  if (why) *why = nullptr;
  if (streamActive || livePushActive) {
    if (why) *why = "Close the live stream first";
    return true;
  }
  if (otaInProgress) {
    if (why) *why = "Wait for the firmware update to finish";
    return true;
  }
  return false;
}

bool kerryUploadInProgress() { return uploadInProgress; }

bool kerryTryBeginUpload() {
  bool ok = false;
  portENTER_CRITICAL(&s_workMux);
  if (!uploadInProgress && !s_ecoNet && !s_portalNet && !streamActive && !livePushActive && !otaInProgress) {
    uploadInProgress = true;
    ok = true;
  }
  portEXIT_CRITICAL(&s_workMux);
  return ok;
}

void kerryEndUpload() {
  portENTER_CRITICAL(&s_workMux);
  uploadInProgress = false;
  portEXIT_CRITICAL(&s_workMux);
}

bool kerryTryBeginEcoNet() {
  bool ok = false;
  portENTER_CRITICAL(&s_workMux);
  if (!uploadInProgress && !s_ecoNet && !s_portalNet && !livePushActive && !otaInProgress) {
    s_ecoNet = true;
    ok = true;
  }
  portEXIT_CRITICAL(&s_workMux);
  return ok;
}

void kerryEndEcoNet() {
  portENTER_CRITICAL(&s_workMux);
  s_ecoNet = false;
  portEXIT_CRITICAL(&s_workMux);
}

static bool beginPortalNet() {
  bool ok = false;
  portENTER_CRITICAL(&s_workMux);
  if (!uploadInProgress && !s_ecoNet && !s_portalNet && !streamActive && !livePushActive && !otaInProgress) {
    s_portalNet = true;
    ok = true;
  }
  portEXIT_CRITICAL(&s_workMux);
  return ok;
}

static void endPortalNet() {
  portENTER_CRITICAL(&s_workMux);
  s_portalNet = false;
  portEXIT_CRITICAL(&s_workMux);
}

static void setStatus(const char *msg) {
  bool changed = false;
  portENTER_CRITICAL(&statusMux);
  if (strcmp(portalStatus, msg) != 0) {
    strncpy(portalStatus, msg, sizeof(portalStatus) - 1);
    portalStatus[sizeof(portalStatus) - 1] = '\0';
    changed = true;
  }
  portEXIT_CRITICAL(&statusMux);
  if (changed) logf("%s", msg);
}

static void copyStatus(char *dest, size_t len) {
  portENTER_CRITICAL(&statusMux);
  strncpy(dest, portalStatus, len - 1);
  dest[len - 1] = '\0';
  portEXIT_CRITICAL(&statusMux);
}

static void applyFlash() {
  if (LED_GPIO < 0) return;
  pinMode(LED_GPIO, OUTPUT);
  digitalWrite(LED_GPIO, flashOn ? LED_ON_LEVEL : !LED_ON_LEVEL);
}

static int clampCamLevel(int value) {
  if (value < -2) return -2;
  if (value > 2) return 2;
  return value;
}

static const char *cameraSensorName() {
  if (isOV3660) return "OV3660";
  if (isOV5640) return "OV5640";
  return "OV2640";
}

static void noteCameraSensor(const sensor_t *sensor) {
  const uint16_t pid = sensor ? sensor->id.PID : 0;
  isOV5640 = pid == OV5640_PID;
  isOV3660 = pid == OV3660_PID;
}

// Any sensor may have saved these. The probe runs after loadConfig().
static bool frameSizeKnown(framesize_t size) {
  return size == FRAMESIZE_QVGA || size == FRAMESIZE_VGA || size == FRAMESIZE_SVGA ||
         size == FRAMESIZE_XGA || size == FRAMESIZE_HD || size == FRAMESIZE_SXGA ||
         size == FRAMESIZE_UXGA || size == FRAMESIZE_FHD || size == FRAMESIZE_QXGA;
}

static bool frameSizeAllowed(framesize_t size) {
  if (size == FRAMESIZE_QVGA || size == FRAMESIZE_VGA || size == FRAMESIZE_SVGA ||
      size == FRAMESIZE_XGA || size == FRAMESIZE_HD)
    return true;
  if ((isOV5640 || isOV3660) && (size == FRAMESIZE_SXGA || size == FRAMESIZE_UXGA)) return true;
  if (isOV3660 && (size == FRAMESIZE_FHD || size == FRAMESIZE_QXGA)) return true;
  return false;
}

static bool psramLimitExceeded(framesize_t size) { return !psramFound() && size > FRAMESIZE_VGA; }

static const framesize_t kSelectableSizes[] = {FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XGA,
                                               FRAMESIZE_HD,   FRAMESIZE_FHD,  FRAMESIZE_SXGA, FRAMESIZE_UXGA,
                                               FRAMESIZE_QXGA};
static_assert(sizeof(kSelectableSizes) / sizeof(kSelectableSizes[0]) == KERRY_SELECTABLE_SIZES_MAX,
              "kSelectableSizes length must match KERRY_SELECTABLE_SIZES_MAX");

static bool frameSizeFallback(framesize_t size) {
  return size == FRAMESIZE_SXGA || size == FRAMESIZE_UXGA || size == FRAMESIZE_FHD ||
         size == FRAMESIZE_QXGA;
}

static framesize_t applyPsramFrameLimit(framesize_t size) {
  return psramLimitExceeded(size) ? FRAMESIZE_VGA : size;
}

static framesize_t effectiveFrameSize() { return applyPsramFrameLimit(camFrameSize); }

int kerryImageSelectableSizes(framesize_t *out, int cap) {
  int n = 0;
  for (framesize_t size : kSelectableSizes) {
    if (psramLimitExceeded(size) || !frameSizeAllowed(size)) continue;
    if (n < cap) {
      if (out) out[n] = size;
      n++;
    }
  }
  return n;
}

static void ov5640SetCaptureFramesize(sensor_t *sensor, framesize_t fs, bool refocusAfterResize);
static void discardCameraFrames(int count);

static void applyCameraImageSettings() {
  if (!cameraReady) return;
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;
  if (sensor->set_brightness) sensor->set_brightness(sensor, camBrightness);
  if (sensor->set_contrast) sensor->set_contrast(sensor, camContrast);
  if (sensor->set_saturation) sensor->set_saturation(sensor, camSaturation);
  // set_framesize reprograms the OV5640 PLL. Doing that after the focus
  // firmware is running drops the lens back to its resting position.
  framesize_t target = effectiveFrameSize();
  if (sensor->set_framesize && (int)sensor->status.framesize != (int)target) {
    if (ov5640SketchActive)
      ov5640SetCaptureFramesize(sensor, target, true);
    else {
      sensor->set_framesize(sensor, target);
      if (sensor->id.PID == OV2640_PID) {
        const char *defErr = kerryOv2640CaptureDefaults(sensor);
        if (defErr) logw("%s", defErr);
      }
    }
  }
}

static void applyCameraFlip() {
  if (!cameraReady) return;
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;
  if (sensor->set_hmirror) sensor->set_hmirror(sensor, flipH ? 1 : 0);
  if (sensor->set_vflip) sensor->set_vflip(sensor, flipV ? 1 : 0);
}

static void applyCameraControls() {
  applyCameraImageSettings();
  const char *expErr = kerryCamExposureApply(true);
  if (expErr) logw("exposure apply failed: %s", expErr);
  applyCameraFlip();
  kerryAwbApply();
}

static bool lockCam(uint32_t ms) {
  if (xSemaphoreTake(camLock, pdMS_TO_TICKS(ms)) != pdTRUE) return false;
  camLockHeld = true;
  return true;
}

static void unlockCam() {
  if (!camLockHeld) return;
  camLockHeld = false;
  xSemaphoreGive(camLock);
}

const char *kerryCamReapplyExposure() {
  if (!lockCam(3000)) return "Camera busy";
  if (!cameraReady) {
    unlockCam();
    return "Camera not ready";
  }
  const char *err = kerryCamExposureApply(true);
  unlockCam();
  return err;
}

static void serviceExposureRevertIfNeeded() {
  if (!kerryCamExposureRevertPending()) return;
  if (!lockCam(2000)) return;
  kerryCamExposureServiceRevert();
  unlockCam();
}

static void cameraRail(bool on) {
  if (CAM_POWER_GPIO < 0) return;
  pinMode(CAM_POWER_GPIO, OUTPUT);
  digitalWrite(CAM_POWER_GPIO, on ? LOW : HIGH);
}

// Stock HP10 PWDN ends HIGH (0→1). Modern esp32-camera inverts that if pin_pwdn=32.
static void cameraPwdnRelease(int endLevel, int firstMs, int secondMs) {
  if (PWDN_GPIO < 0) {
    delay(firstMs + secondMs);
    return;
  }
  pinMode(PWDN_GPIO, OUTPUT);
  digitalWrite(PWDN_GPIO, endLevel ? LOW : HIGH);
  delay(firstMs);
  digitalWrite(PWDN_GPIO, endLevel ? HIGH : LOW);
  delay(secondMs);
}

static void cameraInitCleanup() {
  ov5640SketchActive = false;
  esp_camera_deinit();
  gpio_reset_pin((gpio_num_t)XCLK_GPIO);
}

static bool startCamera();
static bool startCameraWithPwdn(int pwdnEnd, bool forceStandard = false);

static void fillCameraConfig(camera_config_t *config, framesize_t frameSize, int jpegQuality, int fbCount,
                             camera_grab_mode_t grabMode) {
  memset(config, 0, sizeof(*config));
  config->pin_pwdn = -1;
  config->pin_reset = RESET_GPIO;
  config->pin_xclk = XCLK_GPIO;
  config->pin_sccb_sda = SIOD_GPIO;
  config->pin_sccb_scl = SIOC_GPIO;
  config->pin_d7 = Y9_GPIO;
  config->pin_d6 = Y8_GPIO;
  config->pin_d5 = Y7_GPIO;
  config->pin_d4 = Y6_GPIO;
  config->pin_d3 = Y5_GPIO;
  config->pin_d2 = Y4_GPIO;
  config->pin_d1 = Y3_GPIO;
  config->pin_d0 = Y2_GPIO;
  config->pin_vsync = VSYNC_GPIO;
  config->pin_href = HREF_GPIO;
  config->pin_pclk = PCLK_GPIO;
  config->xclk_freq_hz = XCLK_FREQ_HZ;
  config->ledc_timer = LEDC_TIMER_0;
  config->ledc_channel = LEDC_CHANNEL_0;
  config->pixel_format = PIXFORMAT_JPEG;
  config->jpeg_quality = jpegQuality;
  config->sccb_i2c_port = 0;
  config->jpeg_buffer_size = 0;
#if CONFIG_CAMERA_CONVERTER_ENABLED
  config->conv_mode = CONV_DISABLE;
#endif
  config->frame_size = frameSize;
  if (config->frame_size == FRAMESIZE_QSXGA) config->jpeg_buffer_size = (2560UL * 1920UL) / 2;
  else if (config->frame_size == FRAMESIZE_QXGA) config->jpeg_buffer_size = (2048UL * 1536UL) / 2;
  else if (config->frame_size == FRAMESIZE_FHD) config->jpeg_buffer_size = (1920UL * 1080UL) / 2;
  config->fb_count = fbCount;
  config->grab_mode = grabMode;
  config->fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
}

static bool bootCamera(const camera_config_t *config, int pwdnEnd) {
  cameraRail(true);
  if (pwdnEnd)
    cameraPwdnRelease(1, 200, 500);
  else {
    cameraPwdnRelease(0, 10, 10);
    delay(50);
  }

  applyFlash();
  esp_err_t err = esp_camera_init(config);
  if (err != ESP_OK) {
    logf("camera init failed: 0x%x (pwdn=%d)", err, pwdnEnd);
    cameraInitCleanup();
    return false;
  }
  return true;
}

static camera_fb_t *camFbGetBlocking(bool noteHealth) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (noteHealth) {
    if (fb)
      kerryCamExposureNoteCaptureOk();
    else
      kerryCamExposureNoteCaptureFail();
  }
  return fb;
}

static uint32_t grabLockDeadlineMs() {
  uint32_t period = kerryCamExposureFrameMs();
  uint32_t deadline = millis() + 5000UL + period * 2UL;
  if ((uint32_t)(deadline - millis()) > 30000UL) deadline = millis() + 30000UL;
  return deadline;
}

// Caller must hold camLock. Lock is released for the blocking fb_get, then reacquired.
static camera_fb_t *camFbGetHeldLock(bool noteHealth) {
  camGrabActive = true;
  unlockCam();
  camera_fb_t *fb = camFbGetBlocking(noteHealth);
  uint32_t deadline = grabLockDeadlineMs();
  while (!lockCam(200)) {
    if ((int32_t)(millis() - deadline) >= 0) {
      logw("cam lock reacquire failed after fb_get");
      camGrabActive = false;
      return fb;
    }
    delay(10);
  }
  camGrabActive = false;
  return fb;
}

static bool cameraHoldsStaleFrame() {
  return g_activeFbCount == 1 && g_activeGrabMode == CAMERA_GRAB_WHEN_EMPTY;
}

static void logDeliveredFrameAge(const char *why, const camera_fb_t *fb) {
  if (!fb) return;
  int64_t nowUs = esp_timer_get_time();
  int64_t fbUs = (int64_t)fb->timestamp.tv_sec * 1000000LL + (int64_t)fb->timestamp.tv_usec;
  uint32_t ageMs = 0;
  if (nowUs > fbUs) ageMs = (uint32_t)((nowUs - fbUs) / 1000LL);
  logd("%s frame age %ums", why ? why : "capture", (unsigned)ageMs);
}

static void dropHeldStaleFrame() {
  if (!cameraHoldsStaleFrame()) return;
  camera_fb_t *stale = camFbGetHeldLock(false);
  if (stale) esp_camera_fb_return(stale);
}

static void noteActiveGrab(const camera_config_t *config) {
  if (!config) return;
  g_activeFbCount = config->fb_count;
  g_activeGrabMode = config->grab_mode;
}

static void discardCameraFrames(int count) { kerryCamDiscardFrames(count); }

static bool waitForOv5640Focus(uint32_t timeoutMs) {
  const uint32_t started = millis();
  uint8_t status = 0;
  while (millis() - started < timeoutMs) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    status = ov5640Af.getFWStatus();
    // Same as ov5640_ap_snapshot.ino: 0x10 idle; many modules report 0x20 in continuous AF.
    if (status == 0x10 || status == 0x20) {
      logf("OV5640 focus settled: FW_STATUS=0x%02X at %lums", status, (unsigned long)(millis() - started));
      return true;
    }
    delay(100);
  }
  logf("WARN: OV5640 focus not settled; last FW_STATUS=0x%02X", status);
  return false;
}

static bool triggerOv5640SingleFocus(sensor_t *s) {
  if (!s || !s->set_reg || s->set_reg(s, 0x3022, 0xFF, 0x03) != 0) return false;
  logf("OV5640 single focus triggered");
  return true;
}

static void ov5640SetCaptureFramesize(sensor_t *sensor, framesize_t fs, bool refocusAfterResize) {
  if (!sensor || !sensor->set_framesize || (int)sensor->status.framesize == (int)fs) return;
  if (sensor->set_framesize(sensor, fs) != 0) {
    logf("WARN: cannot set capture framesize");
    return;
  }
  // Match the working snapshot sketch: one resize after AF, then discard stale frames only.
  discardCameraFrames(refocusAfterResize ? 3 : 6);
  if (refocusAfterResize && triggerOv5640SingleFocus(sensor)) (void)waitForOv5640Focus(4000);
  logf("OV5640 capture framesize %d", (int)fs);
}

static void initOv5640AutoFocus() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || s->id.PID != OV5640_PID) {
    logf("PID=0x%04X (not OV5640), AF skipped", s ? s->id.PID : 0);
    return;
  }

  // The working sketch focuses at SXGA, then switches to the capture size once.
  framesize_t capture = effectiveFrameSize();
  if (s->status.framesize != OV5640_AF_FRAMESIZE) {
    if (!s->set_framesize || s->set_framesize(s, OV5640_AF_FRAMESIZE) != 0) {
      logf("WARN: OV5640 AF setup skipped");
      return;
    }
  }
  discardCameraFrames(3);

  logf("OV5640 detected, initializing auto-focus at SXGA");
  kerryWdtLenient();
  if (!ov5640Af.start(s)) {
    logf("WARN: OV5640 AF setup skipped");
    kerryWdtRestore();
    return;
  }
  int r1 = ov5640Af.focusInit();
  logf("focusInit=%d status=0x%02X", r1, ov5640Af.getFWStatus());
  if (r1 != 0) {
    logf("ERROR: OV5640 AF setup failed");
    kerryWdtRestore();
    return;
  }
  int r2 = ov5640Af.autoFocusMode();
  logf("autoFocusMode=%d", r2);
  bool focused = r2 == 0 && waitForOv5640Focus(OV5640_FOCUS_TIMEOUT_MS);
  if (!focused && triggerOv5640SingleFocus(s)) focused = waitForOv5640Focus(OV5640_FOCUS_TIMEOUT_MS);
  if (!focused) logf("WARN: OV5640 focus did not settle");

  if (capture != OV5640_AF_FRAMESIZE) ov5640SetCaptureFramesize(s, capture, false);
  discardCameraFrames(2);
  kerryWdtRestore();
}

static bool startCameraWithPwdn(int pwdnEnd, bool forceStandard) {
  ov5640SketchActive = false;
#if defined(ARDUINO_XIAO_ESP32S3)
  const bool sketch = !forceStandard && psramFound();
#else
  const bool sketch = !forceStandard && isOV5640 && psramFound();
#endif
  camera_config_t config;
  if (sketch) {
    fillCameraConfig(&config, OV5640_INIT_FRAMESIZE, 15, 2, CAMERA_GRAB_LATEST);
  } else {
#if defined(ARDUINO_XIAO_ESP32S3)
    fillCameraConfig(&config, effectiveFrameSize(), 15, 2, CAMERA_GRAB_LATEST);
    if (psramFound() && effectiveFrameSize() >= FRAMESIZE_SXGA)
      config.jpeg_buffer_size = (1600UL * 1200UL) / 2;
#else
    fillCameraConfig(&config, effectiveFrameSize(), 12, 1, CAMERA_GRAB_WHEN_EMPTY);
#endif
  }

  if (!bootCamera(&config, pwdnEnd)) {
    if (sketch) {
      logw("OV5640 snapshot init failed, using standard init");
      return startCameraWithPwdn(pwdnEnd, true);
    }
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  noteCameraSensor(sensor);
  if (!frameSizeAllowed(camFrameSize)) {
    logf("resolution %d is not supported on %s, using HD", (int)camFrameSize, cameraSensorName());
    camFrameSize = FRAMESIZE_HD;
    cameraInitCleanup();
    cameraReady = false;
    return startCamera();
  }

  if (sketch && !isOV5640) {
    logf("camera is %s, using standard init", cameraSensorName());
    cameraInitCleanup();
    cameraReady = false;
    return startCameraWithPwdn(pwdnEnd, true);
  }

  if (!sketch && isOV5640 && psramFound() && !forceStandard) {
    logf("OV5640 detected, switching to snapshot init");
    cameraInitCleanup();
    cameraReady = false;
    delay(100);
    return startCameraWithPwdn(pwdnEnd, false);
  }

  ov5640SketchActive = sketch && isOV5640 && psramFound() && !forceStandard;
  logf("camera ready (%s), PSRAM %s pwdn=%d%s", cameraSensorName(), psramFound() ? "yes" : "no", pwdnEnd,
       ov5640SketchActive ? " ov5640-sketch" : "");
  cameraReady = true;
  kerryHealthNoteCameraOk();
  applyFlash();
  if (ov5640SketchActive) initOv5640AutoFocus();
  if (sensor && sensor->id.PID == OV2640_PID) {
    kerryOv2640ForgetDefaults();
    const char *defErr = kerryOv2640CaptureDefaults(sensor);
    if (defErr) logw("%s", defErr);
  }
  noteActiveGrab(&config);
  applyCameraControls();
  return true;
}

static bool startCamera() {
  if (CAM_EN_GPIO >= 0) {
    pinMode(CAM_EN_GPIO, OUTPUT);
    digitalWrite(CAM_EN_GPIO, HIGH);
  }
  cameraRail(true);
  delay(200);
  if (startCameraWithPwdn(1)) return true;
  if (PWDN_GPIO >= 0) {
    logw("camera init failed with stock PWDN=1, trying PWDN=0");
    if (startCameraWithPwdn(0)) return true;
  }
  if (frameSizeFallback(camFrameSize)) {
    logf("camera init failed at resolution %d, retrying HD", (int)camFrameSize);
    camFrameSize = FRAMESIZE_HD;
    return startCamera();
  }
  cameraReady = false;
  isOV5640 = false;
  isOV3660 = false;
  kerryHealthNoteCameraFail("camera_init");
  return false;
}

static void loadConfig() {
  Preferences prefs;
  prefs.begin("kerry", false);
  wifiSsid = prefs.isKey("ssid") ? prefs.getString("ssid", "") : "";
  wifiPass = prefs.isKey("wpass") ? prefs.getString("wpass", "") : "";
  portalUser = prefs.isKey("plogin") ? prefs.getString("plogin", "") : "";
  portalPass = prefs.isKey("ppass") ? prefs.getString("ppass", "") : "";
  termsAccepted = prefs.isKey("terms") ? prefs.getBool("terms", false) : false;
  if (prefs.isKey("framesz")) {
    framesize_t size = (framesize_t)prefs.getUChar("framesz", FRAMESIZE_SVGA);
    if (frameSizeKnown(size)) camFrameSize = size;
  }
  camBrightness = clampCamLevel(prefs.getChar("bright", 0));
  camContrast = clampCamLevel(prefs.getChar("contr", 0));
  camSaturation = clampCamLevel(prefs.getChar("sat", 0));
  flipH = prefs.getBool("flipH", false);
  flipV = prefs.getBool("flipV", false);
  uploadIntervalSec = prefs.getUInt("upint", 0);
  if (uploadIntervalSec > UPLOAD_INTERVAL_MAX_SEC) uploadIntervalSec = 0;
  if (prefs.isKey("useportal")) {
    usePortal = prefs.getBool("useportal", false);
  } else {
    usePortal = portalUser.length() > 0;
  }
  prefs.end();
}

static void saveUploadSettings() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putUInt("upint", uploadIntervalSec);
  prefs.end();
}

static void saveImageSettings() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putUChar("framesz", (uint8_t)camFrameSize);
  prefs.putChar("bright", camBrightness);
  prefs.putChar("contr", camContrast);
  prefs.putChar("sat", camSaturation);
  prefs.putBool("flipH", flipH);
  prefs.putBool("flipV", flipV);
  prefs.end();
}

static void saveConfig() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("wpass", wifiPass);
  prefs.putString("plogin", portalUser);
  prefs.putString("ppass", portalPass);
  prefs.putBool("terms", termsAccepted);
  prefs.putBool("useportal", usePortal);
  prefs.end();
}

void kerryCopyStatus(char *dest, size_t len) { copyStatus(dest, len); }

void kerrySaveConfig() { saveConfig(); }

void kerrySaveUploadSettings() { saveUploadSettings(); }

static bool shipWifiUp() {
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) return false;
  return wifiStaHasIp || WiFi.status() == WL_CONNECTED;
}

void kerryNetworkFillJson(JsonDocument &doc) {
  char status[160];
  copyStatus(status, sizeof(status));

  doc["ssid"] = wifiSsid;
  doc["portalUser"] = portalUser;
  doc["termsAccepted"] = termsAccepted;
  doc["usePortal"] = usePortal;
  doc["uploadIntervalSec"] = uploadIntervalSec;
  doc["uploadStatus"] = uploadStatus;
  doc["apSsid"] = AP_SSID;
  doc["apUrl"] = "http://" + AP_IP.toString() + "/";
  doc["statusText"] = status;
  doc["portalState"] = portalOk ? "Open" : "Closed";

  if (shipWifiUp()) {
    doc["wifiIp"] = WiFi.localIP().toString();
    doc["wifiGateway"] = WiFi.gatewayIP().toString();
    doc["wifiSubnet"] = WiFi.subnetMask().toString();
    doc["wifiRssi"] = WiFi.RSSI();
    doc["connectedSsid"] = WiFi.SSID();
    doc["wifiChannel"] = WiFi.channel();
    if (portalOk) {
      doc["wifiStatus"] = "Connected, internet open";
    } else if (usePortal && portalUser.length()) {
      doc["wifiStatus"] = "Connected, portal sign-in pending";
    } else {
      doc["wifiStatus"] = "Connected";
    }
  } else if (wifiSsid.length()) {
    doc["wifiIp"] = "—";
    doc["wifiGateway"] = "—";
    doc["wifiSubnet"] = "—";
    doc["connectedSsid"] = "";
    doc["wifiStatus"] = wifiStaWasConnected ? "Reconnecting" : "Joining";
  } else {
    doc["wifiIp"] = "—";
    doc["wifiGateway"] = "—";
    doc["wifiSubnet"] = "—";
    doc["connectedSsid"] = "";
    doc["wifiStatus"] = "Not configured";
  }
}

struct kerry_scan_row_t {
  String ssid;
  int rssi;
  uint8_t auth;
  uint8_t ch;
};

static void scanAddRow(String ssid, int rssi, uint8_t auth, uint8_t ch, kerry_scan_row_t *rows, int *count) {
  ssid.trim();
  if (!ssid.length() || *count >= 24) return;
  for (int j = 0; j < *count; j++) {
    if (rows[j].ssid == ssid) {
      if (rssi > rows[j].rssi) {
        rows[j].rssi = rssi;
        rows[j].auth = auth;
        rows[j].ch = ch;
      }
      return;
    }
  }
  rows[*count].ssid = ssid;
  rows[*count].rssi = rssi;
  rows[*count].auth = auth;
  rows[*count].ch = ch;
  (*count)++;
}

const char *kerryWifiScan(JsonDocument &doc) {
  if (streamActive) return "Close the live stream first";
  if (uploadInProgress || kerryEcoNetBusy()) return "Wait for the upload to finish";
  if (otaInProgress) return "Wait for the firmware update to finish";

  kerry_scan_row_t rows[24];
  int count = 0;

  wifi_scan_config_t sc = {};
  sc.show_hidden = true;
  sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  sc.scan_time.active.min = 120;
  sc.scan_time.active.max = 450;

  disableLoopWDT();
  esp_wifi_scan_stop();
  esp_err_t err = ESP_FAIL;
  for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
    if (attempt) vTaskDelay(pdMS_TO_TICKS(400));
    err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) logd("wifi scan start attempt %d: %s", attempt + 1, esp_err_to_name(err));
  }
  enableLoopWDT();

  if (err == ESP_OK) {
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    logd("wifi scan found %u raw AP(s)", (unsigned)n);
    if (n > 32) n = 32;
    if (n > 0) {
      wifi_ap_record_t *aps = (wifi_ap_record_t *)malloc(n * sizeof(wifi_ap_record_t));
      if (aps) {
        uint16_t want = n;
        if (esp_wifi_scan_get_ap_records(&want, aps) == ESP_OK) {
          for (uint16_t i = 0; i < want; i++) {
            if (!aps[i].ssid[0]) continue;
            scanAddRow(String((const char *)aps[i].ssid), aps[i].rssi, aps[i].authmode, aps[i].primary, rows,
                       &count);
          }
        }
        free(aps);
      }
    }
  } else {
    logw("wifi scan failed: %s", esp_err_to_name(err));
  }

  if (shipWifiUp()) {
    String cur = WiFi.SSID();
    if (cur.length()) {
      wifi_ap_record_t curAp = {};
      if (esp_wifi_sta_get_ap_info(&curAp) == ESP_OK) {
        scanAddRow(cur, curAp.rssi, curAp.authmode, curAp.primary, rows, &count);
      } else {
        scanAddRow(cur, WiFi.RSSI(), WIFI_AUTH_WPA2_PSK, (uint8_t)WiFi.channel(), rows, &count);
      }
    }
  }
  if (wifiSsid.length()) {
    scanAddRow(wifiSsid, -100, WIFI_AUTH_WPA2_PSK, 0, rows, &count);
  }

  if (count == 0) {
    if (err != ESP_OK) return "Wi-Fi scan failed (try again in a few seconds)";
    return "No networks found";
  }

  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (rows[j].rssi > rows[i].rssi) {
        kerry_scan_row_t tmp = rows[i];
        rows[i] = rows[j];
        rows[j] = tmp;
      }
    }
  }

  JsonArray list = doc["networks"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    JsonObject row = list.add<JsonObject>();
    row["ssid"] = rows[i].ssid;
    row["rssi"] = rows[i].rssi;
    row["auth"] = rows[i].auth;
    row["channel"] = rows[i].ch;
    row["secure"] = rows[i].auth != WIFI_AUTH_OPEN;
  }
  doc["count"] = count;
  return nullptr;
}

bool kerryStreamActive() { return streamActive; }

bool kerryCamGrabActive() { return camGrabActive; }

bool kerryCamTelemetryGrabCopy(uint8_t **jpeg, size_t *len) {
  if (!jpeg || !len) return false;
  *jpeg = nullptr;
  *len = 0;
  if (!cameraReady) return false;
  if (!lockCam(3000)) return false;
  kerryWdtLenient();
  camera_fb_t *fb = camFbGetHeldLock(false);
  if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0) {
    if (fb) esp_camera_fb_return(fb);
    kerryWdtRestore();
    unlockCam();
    return false;
  }
  uint8_t *copy = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) copy = (uint8_t *)malloc(fb->len);
  if (!copy) {
    esp_camera_fb_return(fb);
    kerryWdtRestore();
    unlockCam();
    return false;
  }
  memcpy(copy, fb->buf, fb->len);
  *len = fb->len;
  *jpeg = copy;
  esp_camera_fb_return(fb);
  kerryWdtRestore();
  unlockCam();
  return true;
}

void kerryCamTelemetryFreeCopy(uint8_t *jpeg) { free(jpeg); }

int kerryCamDiscardFrames(int count, uint32_t *tsMs, int tsCap) {
  if (count <= 0 || !camLockHeld) return 0;
  int got = 0;
  for (int i = 0; i < count; i++) {
    camGrabActive = true;
    unlockCam();
    camera_fb_t *fb = camFbGetBlocking(false);
    uint32_t stamp = 0;
    bool have = false;
    if (fb) {
      int64_t us = (int64_t)fb->timestamp.tv_sec * 1000000LL + (int64_t)fb->timestamp.tv_usec;
      if (us > 0) stamp = (uint32_t)(us / 1000LL);
      have = true;
      esp_camera_fb_return(fb);
    }
    delay(40);
    uint32_t deadline = grabLockDeadlineMs();
    while (!lockCam(200)) {
      if ((int32_t)(millis() - deadline) >= 0) {
        camGrabActive = false;
        logw("discard: cam lock reacquire failed");
        if (have) {
          if (tsMs && got < tsCap) tsMs[got] = stamp;
          got++;
        }
        return got;
      }
      delay(10);
    }
    camGrabActive = false;
    if (!have) break;
    if (tsMs && got < tsCap) tsMs[got] = stamp;
    got++;
  }
  return got;
}

bool kerryCameraReady() { return cameraReady; }

const char *kerryCameraSensorName() {
  if (!cameraReady) return "Not detected";
  return cameraSensorName();
}

static const char *frameSizeLabel(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QVGA:
      return "QVGA 320×240";
    case FRAMESIZE_VGA:
      return "VGA 640×480";
    case FRAMESIZE_SVGA:
      return "SVGA 800×600";
    case FRAMESIZE_XGA:
      return "XGA 1024×768";
    case FRAMESIZE_HD:
      return "HD 1280×720";
    case FRAMESIZE_SXGA:
      return "SXGA 1280×1024";
    case FRAMESIZE_UXGA:
      return "UXGA 1600×1200";
    case FRAMESIZE_FHD:
      return "FHD 1920×1080";
    case FRAMESIZE_QXGA:
      return "QXGA 2048×1536";
    default:
      return "Unknown";
  }
}

void kerryImageGet(JsonDocument &doc) {
  doc["framesize"] = (int)camFrameSize;
  doc["framesizeLabel"] = frameSizeLabel(camFrameSize);
  doc["brightness"] = camBrightness;
  doc["contrast"] = camContrast;
  doc["saturation"] = camSaturation;
  doc["flipH"] = flipH;
  doc["flipV"] = flipV;
  doc["hasPsram"] = psramFound();
  doc["streamActive"] = streamActive;
  doc["cameraReady"] = cameraReady;
  doc["sensor"] = cameraReady ? cameraSensorName() : "unknown";
  JsonArray sizes = doc["sizes"].to<JsonArray>();
  framesize_t listed[KERRY_SELECTABLE_SIZES_MAX];
  int n = kerryImageSelectableSizes(listed, KERRY_SELECTABLE_SIZES_MAX);
  for (int i = 0; i < n; i++) {
    JsonObject row = sizes.add<JsonObject>();
    row["value"] = (int)listed[i];
    row["label"] = frameSizeLabel(listed[i]);
  }
  kerryCamExposureFillJson(doc);
}

const char *kerryImageResolveFramesize(int framesize, framesize_t *requested, framesize_t *effective) {
  framesize_t newSize = (framesize < 0) ? camFrameSize : (framesize_t)framesize;
  if (framesize >= 0) {
    if (!frameSizeAllowed(newSize)) return "Unsupported resolution";
    if (psramLimitExceeded(newSize)) return "No PSRAM for that resolution";
  }
  if (requested) *requested = newSize;
  if (effective) *effective = applyPsramFrameLimit(newSize);
  return nullptr;
}

const char *kerryImageSet(int framesize, int brightness, int contrast, int saturation, int flipHSet, int flipVSet) {
  if (streamActive) return "Close the live stream first";
  if (camGrabActive) return "Capture in progress, try again";

  framesize_t newSize = FRAMESIZE_INVALID;
  const char *fsErr = kerryImageResolveFramesize(framesize, &newSize, nullptr);
  if (fsErr) return fsErr;
  bool resolutionChanged = newSize != camFrameSize;
  if (framesize >= 0) camFrameSize = newSize;
  if (brightness >= -2) camBrightness = clampCamLevel(brightness);
  if (contrast >= -2) camContrast = clampCamLevel(contrast);
  if (saturation >= -2) camSaturation = clampCamLevel(saturation);
  if (flipHSet >= 0) flipH = flipHSet != 0;
  if (flipVSet >= 0) flipV = flipVSet != 0;
  saveImageSettings();

  if (!lockCam(3000)) return "Camera busy";

  bool ok;
  if (resolutionChanged || !cameraReady) {
    if (cameraReady) {
      esp_camera_deinit();
      cameraReady = false;
    }
    ok = startCamera();
  } else {
    applyCameraControls();
    ok = cameraReady;
  }
  const char *applyErr = kerryCamExposureLastApplyError();
  unlockCam();

  if (!ok) return "Camera rejected settings";
  if (applyErr) return applyErr;
  logf("image resolution=%d bright=%d contrast=%d saturation=%d flipH=%d flipV=%d", (int)camFrameSize,
       camBrightness, camContrast, camSaturation, flipH ? 1 : 0, flipV ? 1 : 0);
  return nullptr;
}

static String urlDecode(const String &in) {
  String out;
  out.reserve(in.length());
  for (int i = 0; i < (int)in.length(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < (int)in.length()) {
      char hex[3] = {in[i + 1], in[i + 2], 0};
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

static String urlEncode(const String &in) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(in.length() * 3);
  for (size_t i = 0; i < in.length(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

static String formValue(const String &body, const char *key) {
  String needle = String(key) + "=";
  int p = 0;
  while (p < (int)body.length()) {
    int amp = body.indexOf('&', p);
    if (amp < 0) amp = body.length();
    String pair = body.substring(p, amp);
    if (pair.startsWith(needle)) return urlDecode(pair.substring(needle.length()));
    p = amp + 1;
  }
  return "";
}

static int formInt(const String &body, const char *key, int fallback) {
  String value = formValue(body, key);
  if (!value.length()) return fallback;
  return value.toInt();
}

static String headerValue(const String &headers, const char *name) {
  String low = headers;
  low.toLowerCase();
  String key = String(name);
  key.toLowerCase();
  key += ":";
  int p = low.indexOf(key);
  if (p < 0) return "";
  int start = p + key.length();
  while (start < (int)headers.length() && (headers[start] == ' ' || headers[start] == '\t')) start++;
  int end = headers.indexOf('\r', start);
  if (end < 0) end = headers.length();
  return headers.substring(start, end);
}

static void rememberSession(const String &headers) {
  String low = headers;
  low.toLowerCase();
  int p = low.indexOf("phpsessid=");
  if (p < 0) return;
  p += 10;
  size_t i = 0;
  while (p < (int)headers.length() && i + 1 < sizeof(sessionId)) {
    char c = headers[p++];
    if (c == ';' || c == '\r' || c == ' ' || c == '\n') break;
    sessionId[i++] = c;
  }
  sessionId[i] = '\0';
}

static bool readHeaders(WiFiClientSecure &client, String &headers) {
  uint32_t start = millis();
  headers = "";
  while (millis() - start < 12000) {
    while (client.available()) {
      headers += (char)client.read();
      if (headers.endsWith("\r\n\r\n")) return true;
      if (headers.length() > 3500) return headers.indexOf("\r\n\r\n") >= 0;
    }
    if (!client.connected() && !client.available()) break;
    delay(2);
  }
  return headers.indexOf("\r\n\r\n") >= 0;
}

static WiFiClientSecure &portalTlsClient() {
  static WiFiClientSecure client;
  static bool ready = false;
  if (!ready) {
    client.setInsecure();
    client.setHandshakeTimeout(15000);
    client.setTimeout(15000);
    ready = true;
  } else {
    client.stop();
  }
  return client;
}

static int httpsRequest(const char *method, const String &path, const String &body, bool readBody) {
  WiFiClientSecure &client = portalTlsClient();
  IPAddress resolved;
  char ipTxt[16] = "-";
  bool dnsOk = WiFi.hostByName(PORTAL_HOST, resolved);
  if (dnsOk) snprintf(ipTxt, sizeof ipTxt, "%u.%u.%u.%u", resolved[0], resolved[1], resolved[2], resolved[3]);
  unsigned largest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  logf("portal %s %s -> %s internal largest %u", method, path.c_str(), ipTxt, largest);
  if (!dnsOk) {
    setStatus("Could not resolve the ferry portal");
    return -1;
  }
  if (!client.connect(PORTAL_HOST, 443, 15000)) {
    logw("portal TLS failed to %s internal largest %u", ipTxt,
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    setStatus("Could not reach the ferry portal");
    return -1;
  }

  String req;
  req.reserve(400 + body.length());
  req += method;
  req += " ";
  req += path;
  req += " HTTP/1.0\r\nHost: ";
  req += PORTAL_HOST;
  req += "\r\nUser-Agent: KerryCam\r\nConnection: close\r\n";
  if (sessionId[0]) {
    req += "Cookie: PHPSESSID=";
    req += sessionId;
    req += "\r\n";
  }
  if (strcmp(method, "POST") == 0) {
    req += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ";
    req += String(body.length());
    req += "\r\nX-Requested-With: XMLHttpRequest\r\n";
    req += "Origin: https://internet.stenaline.com\r\n";
    req += "Referer: https://internet.stenaline.com/106/portal/\r\n";
  }
  req += "\r\n";
  client.print(req);
  if (body.length()) client.print(body);

  String headers;
  if (!readHeaders(client, headers)) {
    client.stop();
    setStatus("Ferry portal did not answer");
    return -1;
  }
  rememberSession(headers);
  redirectPath = headerValue(headers, "Location");
  int status = 0;
  int sp = headers.indexOf(' ');
  if (sp > 0) status = headers.substring(sp + 1).toInt();

  portalBody[0] = '\0';
  if (readBody) {
    int contentLength = headerValue(headers, "Content-Length").toInt();
    int limit = (int)sizeof(portalBody) - 1;
    if (contentLength > 0 && contentLength < limit) limit = contentLength;
    int n = 0;
    uint32_t start = millis();
    while (n < limit && millis() - start < 8000) {
      while (client.available() && n < limit) {
        portalBody[n++] = (char)client.read();
        start = millis();
      }
      if (!client.connected() && !client.available()) break;
      delay(2);
    }
    portalBody[n] = '\0';
  }
  client.stop();
  if (status < 200 || status >= 400) {
    logw("portal HTTP %d, session %s%s%s", status, sessionId[0] ? "yes" : "no",
         redirectPath.length() ? ", redirect " : "",
         redirectPath.length() ? redirectPath.c_str() : "");
  } else {
    logd("portal HTTP %d, session %s", status, sessionId[0] ? "yes" : "no");
  }
  if (!sessionId[0]) {
    String shown = headers;
    shown.replace("\r", " ");
    shown.replace("\n", " ");
    if (shown.length() > 220) shown.remove(220);
    logd("portal headers: %s", shown.c_str());
  }
  return status;
}

static bool bodyHas(const char *needle) {
  return portalBody[0] && strstr(portalBody, needle) != nullptr;
}

static bool portalConnectedBody() {
  return bodyHas("\"type\":\"CONNECT\"") || bodyHas("\"type\": \"CONNECT\"");
}

static void notePortalFailure() {
  const char *code = strstr(portalBody, "\"code\":\"");
  if (!code) code = strstr(portalBody, "\"code\": \"");
  if (code) {
    code = strchr(code, ':');
    if (code) {
      code++;
      while (*code == ' ' || *code == '"') code++;
      char msg[120];
      snprintf(msg, sizeof(msg), "Portal refused: %.60s", code);
      char *q = strchr(msg, '"');
      if (q) *q = '\0';
      setStatus(msg);
      return;
    }
  }
  const char *step = strstr(portalBody, "\"step\":\"");
  if (step) {
    step += 8;
    char msg[96];
    snprintf(msg, sizeof(msg), "Portal stopped at %.40s", step);
    char *q = strchr(msg, '"');
    if (q) *q = '\0';
    setStatus(msg);
    return;
  }
  setStatus("Portal login was not accepted");
}

static bool sameHostPath(const String &location, String &path) {
  if (location.startsWith("/")) {
    path = location;
    return true;
  }
  const String prefix = String("https://") + PORTAL_HOST;
  if (location.startsWith(prefix)) {
    path = location.substring(prefix.length());
    if (!path.length() || path.charAt(0) != '/') path = "/" + path;
    return true;
  }
  return false;
}

static bool internetIsOpen() {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, "http://clients3.google.com/generate_204")) return false;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char *keys[] = {"Location"};
  http.collectHeaders(keys, 1);
  int code = http.sendRequest("HEAD");
  probeLocation = http.hasHeader("Location") ? http.header("Location") : "";
  http.end();
  if (code != 204) {
    logd("connectivity probe HTTP %d%s%s", code, probeLocation.length() ? " -> " : "", probeLocation.c_str());
  }
  return code == 204;
}

static bool portalQuotaExhausted() { return bodyHas("error_logon_volume-quota-reached-detail"); }

static bool signInToPortal(bool *quota) {
  if (quota) *quota = false;
  sessionId[0] = '\0';
  String path = "/";
  String fromProbe;
  if (sameHostPath(probeLocation, fromProbe)) path = fromProbe;
  bool gotSession = false;
  for (int hop = 0; hop < 5; hop++) {
    int status = httpsRequest("GET", path, "", false);
    if (status < 0) return false;
    if (sessionId[0]) gotSession = true;
    if ((status == 301 || status == 302) && sameHostPath(redirectPath, path)) continue;
    break;
  }
  if (!gotSession) {
    int status = httpsRequest("GET", "/106/portal/", "", false);
    if (status < 0) return false;
    gotSession = sessionId[0] != '\0';
  }
  if (!gotSession) {
    setStatus("Ferry portal did not start a session");
    return false;
  }

  int status = httpsRequest("POST", "/portal_api.php", "action=init", true);
  if (portalQuotaExhausted()) {
    if (quota) *quota = true;
    setStatus("Portal quota exhausted");
    return false;
  }
  if (status != 200) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Portal init failed (%d)", status);
    setStatus(msg);
    return false;
  }
  if (portalConnectedBody()) {
    setStatus("Portal already connected");
    return true;
  }

  String auth = "action=authenticate&switch_package=true&login=";
  auth += urlEncode(portalUser);
  auth += "&password=";
  auth += urlEncode(portalPass);
  auth += "&policy_accept=true&private_policy_accept=false&from_ajax=true&wispr_mode=false";
  status = httpsRequest("POST", "/portal_api.php", auth, true);
  if (status == 200 && portalConnectedBody()) {
    setStatus("Signed in to the ferry portal");
    return true;
  }
  if (portalQuotaExhausted()) {
    if (quota) *quota = true;
    setStatus("Portal quota exhausted");
    return false;
  }
  notePortalFailure();
  return false;
}

static void maybeNetReboot(const char *why) {
  uint32_t now = millis();
  if (nextNetRebootMs != 0 && (int32_t)(now - nextNetRebootMs) < 0) return;
  nextNetRebootMs = now + NET_REBOOT_RETRY_MS;
  kerryHealthNoteNetFail(why);
}

static void notePortalSession(bool ok) {
  if (!ok) {
    portalOk = false;
    crewAuthenticated = false;
    return;
  }
  portalOk = true;
  crewAuthenticated = true;
  portalAuthFailCycles = 0;
  portalFullCycles = 0;
}

static bool postPortalDisconnect() {
  if (!portalUser.length()) return false;
  String body = "action=disconnect&login=";
  body += urlEncode(portalUser);
  logf("portal disconnect for '%s'", portalUser.c_str());
  int status = httpsRequest("POST", "/portal_api.php", body, true);
  bool ok = status == 200 && bodyHas("disconnect_success");
  if (ok) logf("portal disconnect_success");
  else logw("portal disconnect failed (HTTP %d)", status);
  return ok;
}

static void onPortalAuthFailed(bool quota) {
  portalOk = false;
  crewAuthenticated = false;
  if (!shipWifiUp()) {
    logw("portal auth aborted, ship Wi-Fi down");
    return;
  }
  if (portalAuthFailCycles < 255) portalAuthFailCycles++;
  logf("portal auth fail cycle %u/%u%s", portalAuthFailCycles, PORTAL_AUTH_FAIL_THRESHOLD, quota ? " (quota)" : "");
  if (portalAuthFailCycles < PORTAL_AUTH_FAIL_THRESHOLD) return;

  portalAuthFailCycles = 0;
  setStatus("Disconnecting ferry portal session");
  postPortalDisconnect();
  sessionId[0] = '\0';
  bool quotaAgain = false;
  bool ok = signInToPortal(&quotaAgain);
  notePortalSession(ok);
  if (ok) {
    logf("portal recovered after disconnect");
    return;
  }
  if (portalFullCycles < 255) portalFullCycles++;
  logw("portal disconnect+reauth cycle %u/%u failed%s", portalFullCycles, PORTAL_FULL_CYCLE_REBOOT,
       quotaAgain ? " (quota)" : "");
  if (portalFullCycles >= PORTAL_FULL_CYCLE_REBOOT) maybeNetReboot("portal auth failed");
}

static void maintainPortal() {
  if (!wifiSsid.length()) {
    setStatus("Configure router Wi-Fi on the Network page");
    return;
  }
  if (!shipWifiUp()) {
    setStatus("Waiting for router Wi-Fi");
    return;
  }
  if (!usePortal) {
    if (internetIsOpen()) {
      portalOk = true;
      setStatus("Internet is open");
    } else {
      portalOk = false;
      setStatus("Router Wi-Fi is up, waiting for internet");
    }
    return;
  }
  if (!portalUser.length()) {
    setStatus("Router Wi-Fi is up. Add the portal login on Network");
    return;
  }
  if (!termsAccepted) {
    setStatus("Accept the ferry terms on Network, then save");
    return;
  }

  if (!crewAuthenticated) {
    setStatus("Signing in to the ferry portal");
    bool quota = false;
    bool ok = signInToPortal(&quota);
    notePortalSession(ok);
    if (!ok) onPortalAuthFailed(quota);
    return;
  }

  if (internetIsOpen()) {
    portalOk = true;
    return;
  }
  logw("internet lost while signed in — signing in again");
  crewAuthenticated = false;
  portalOk = false;
  setStatus("Internet lost, signing in again");
  bool quota = false;
  bool ok = signInToPortal(&quota);
  notePortalSession(ok);
  if (!ok) onPortalAuthFailed(quota);
}

static void servicePortal() {
  if (!wifiSsid.length() || !shipWifiUp()) return;
  if (nextPortalMs != 0 && (int32_t)(millis() - nextPortalMs) < 0) return;
  if (!beginPortalNet()) {
    nextPortalMs = millis() + 5000;
    return;
  }
  disableLoopWDT();
  maintainPortal();
  enableLoopWDT();
  endPortalNet();
  nextPortalMs = millis() + (portalOk ? PORTAL_CHECK_AUTH_MS : PORTAL_CHECK_UNAUTH_MS);
}

static uint8_t *allocUploadBuffer(size_t size) {
  uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)malloc(size);
  return buf;
}

static bool overlayHasVisibleElements() {
  const ov_geom_t *geom = kerry_overlay_geom();
  if (!geom) return false;
  for (int i = 0; i < OV_EL_N; i++)
    if (geom->en[i]) return true;
  return false;
}

static bool tryOverlay(const char *why, const uint8_t *jpeg, size_t jpegLen, uint8_t **out, size_t *outLen) {
  *out = nullptr;
  *outLen = 0;
  if (!kerry_overlay_enabled() || !overlayHasVisibleElements()) return false;

  ov_nav_t nav;
  memset(&nav, 0, sizeof nav);
  if (!kerryGeoPulseHasCredentials()) {
    logd("overlay %s skipped GeoPulse (no credentials)", why);
  } else if (WiFi.status() != WL_CONNECTED) {
    logd("overlay %s skipped GeoPulse (ship Wi-Fi down)", why);
  } else {
    disableLoopWDT();
    bool got = kerryGeoPulseEnsureNav(false);
    enableLoopWDT();
    if (!got) logw("overlay %s GeoPulse fetch failed: %s", why, kerryGeoPulseLastError());
  }
  const ov_nav_t *cached = kerryGeoPulseNavForOverlay();
  if (cached && cached->ok) {
    nav = *cached;
    logd("overlay %s nav cache age %lus", why, (unsigned long)(kerryGeoPulseCacheAgeMs() / 1000UL));
  } else {
    logd("overlay %s without GeoPulse data", why);
  }

  uint32_t t0 = millis();
  esp_err_t err = kerry_overlay_render(&nav, jpeg, jpegLen, out, outLen);
  if (err == ESP_OK) {
    logd("overlay %s OK %lums %u->%u bytes", why, millis() - t0, (unsigned)jpegLen, (unsigned)*outLen);
  } else {
    logw("overlay %s failed in %lums (%u bytes in)", why, millis() - t0, (unsigned)jpegLen);
  }
  if (err == ESP_OK) return true;
  if (*out) {
    free(*out);
    *out = nullptr;
    *outLen = 0;
  }
  return false;
}

static const esp_task_wdt_config_t WDT_UPLOAD = {
    .timeout_ms = 180000,
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic = true,
};
static const esp_task_wdt_config_t WDT_DEFAULT = {
    .timeout_ms = 5000,
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic = true,
};

static portMUX_TYPE g_wdtMux = portMUX_INITIALIZER_UNLOCKED;
static int g_wdtDepth = 0;

void kerryWdtLenient() {
  bool enter = false;
  portENTER_CRITICAL(&g_wdtMux);
  if (g_wdtDepth == 0) enter = true;
  g_wdtDepth++;
  portEXIT_CRITICAL(&g_wdtMux);
  if (enter) {
    disableLoopWDT();
    esp_task_wdt_reconfigure(&WDT_UPLOAD);
  }
}

void kerryWdtRestore() {
  bool leave = false;
  bool unmatched = false;
  portENTER_CRITICAL(&g_wdtMux);
  if (g_wdtDepth == 0) {
    unmatched = true;
  } else {
    if (g_wdtDepth == 1) leave = true;
    g_wdtDepth--;
  }
  portEXIT_CRITICAL(&g_wdtMux);
  if (unmatched && kerry_log_every_ms("wdt_restore_unmatched", 60000)) {
    logw("kerryWdtRestore called with no matching lenient section");
  }
  if (leave) {
    esp_task_wdt_reconfigure(&WDT_DEFAULT);
    enableLoopWDT();
  }
}

static void logUploadMem(const char *phase) {
  ulogf("mem %s: heap=%u internal=%u psram=%u", phase,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void logUploadTickBlocked() {
  char snap[96];
  snprintf(snap, sizeof snap, "wifi=%d portal=%d stream=%d ota=%d busy=%d", WiFi.status() == WL_CONNECTED ? 1 : 0,
           portalOk ? 1 : 0, streamActive ? 1 : 0, otaInProgress ? 1 : 0, uploadInProgress ? 1 : 0);
  if (kerry_log_if_changed("upload_blocked", snap)) {
    ulogw("tick due but blocked (%s)", snap);
    return;
  }
  if (kerry_log_every_ms("upload_blocked", 120000)) ulogw("tick still blocked (%s)", snap);
}

bool kerryCaptureUploadJpeg(const char *why, uint8_t **out, size_t *outLen, char *err, size_t errLen) {
  if (out) *out = nullptr;
  if (outLen) *outLen = 0;
  if (!out || !outLen || !err || errLen == 0) return false;
  err[0] = 0;
  if (!why) why = "upload";
  if (!lockCam(5000)) {
    snprintf(err, errLen, "Upload skipped, camera busy");
    return false;
  }
  if (!cameraReady) {
    unlockCam();
    snprintf(err, errLen, "Upload skipped, camera not ready");
    return false;
  }
  if (streamActive || livePushActive) {
    unlockCam();
    snprintf(err, errLen, "Upload skipped, live stream open");
    return false;
  }

  dropHeldStaleFrame();
  camera_fb_t *fb = camFbGetHeldLock(true);
  if (fb) logDeliveredFrameAge(why, fb);
  if (!fb) {
    unlockCam();
    serviceExposureRevertIfNeeded();
    snprintf(err, errLen, "Upload failed, no frame");
    return false;
  }
  size_t jpegLen = fb->len;
  uint8_t *jpeg = allocUploadBuffer(jpegLen);
  if (!jpeg) {
    esp_camera_fb_return(fb);
    unlockCam();
    snprintf(err, errLen, "Upload failed, out of memory");
    return false;
  }
  memcpy(jpeg, fb->buf, jpegLen);
  esp_camera_fb_return(fb);
  unlockCam();

  uint8_t *overlayBuf = nullptr;
  size_t overlayLen = 0;
  if (tryOverlay(why, jpeg, jpegLen, &overlayBuf, &overlayLen)) {
    if (overlayLen <= jpegLen) {
      memcpy(jpeg, overlayBuf, overlayLen);
      jpegLen = overlayLen;
    } else {
      uint8_t *grown = allocUploadBuffer(overlayLen);
      if (grown) {
        memcpy(grown, overlayBuf, overlayLen);
        free(jpeg);
        jpeg = grown;
        jpegLen = overlayLen;
      } else {
        logw("overlay bigger (%u) but realloc failed, using original", (unsigned)overlayLen);
      }
    }
    free(overlayBuf);
  }
  *out = jpeg;
  *outLen = jpegLen;
  return true;
}

static bool uploadCaptureToFinestra() {
  uint32_t t0 = millis();
  if (WiFi.status() != WL_CONNECTED || !portalOk) {
    ulogw("skipped: wifi=%d portal=%d", (int)WiFi.status(), portalOk ? 1 : 0);
    return false;
  }

  ulogf("starting");
  char capErr[96];
  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  if (!kerryCaptureUploadJpeg("upload", &jpeg, &jpegLen, capErr, sizeof capErr)) {
    snprintf(uploadStatus, sizeof uploadStatus, "%s", capErr);
    ulogw("%s (%lums)", capErr, millis() - t0);
    return false;
  }
  uint32_t tCap = millis();

  logUploadMem("pre-http");

  static const char *boundary = "KerryCamUploadBoundary";
  String partHead = String("--") + boundary +
                    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"capture.jpg\"\r\n"
                    "Content-Type: image/jpeg\r\n\r\n";
  String partTail = String("\r\n--") + boundary + "--\r\n";
  size_t bodyLen = partHead.length() + jpegLen + partTail.length();
  uint8_t *body = allocUploadBuffer(bodyLen);
  if (!body) {
    free(jpeg);
    strncpy(uploadStatus, "Upload failed, out of memory", sizeof(uploadStatus));
    ulogw("failed: multipart body OOM (%u bytes, %lums)", (unsigned)bodyLen, millis() - t0);
    return false;
  }

  size_t offset = 0;
  memcpy(body + offset, partHead.c_str(), partHead.length());
  offset += partHead.length();
  memcpy(body + offset, jpeg, jpegLen);
  offset += jpegLen;
  memcpy(body + offset, partTail.c_str(), partTail.length());
  free(jpeg);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10000);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  bool ok = false;
  int code = -1;
  uint32_t tHttp0 = millis();
  ulogf("HTTP POST %u bytes", (unsigned)bodyLen);
  for (int attempt = 0; attempt < 2 && !ok; attempt++) {
    if (attempt > 0) {
      ulogw("HTTP retry (prior code %d)", code);
      delay(2000);
    }
    if (!http.begin(client, UPLOAD_URL)) {
      ulogw("HTTP client could not start");
      code = -1;
      continue;
    }
    http.addHeader("X-API-Key", UPLOAD_API_KEY);
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + boundary);
    code = http.POST(body, bodyLen);
    ok = code >= 200 && code < 300;
    http.end();
  }
  uint32_t tHttp = millis();
  free(body);
  uint32_t tDone = millis();

  if (ok) {
    snprintf(uploadStatus, sizeof(uploadStatus), "Last upload OK (HTTP %d)", code);
    kerryHealthNoteUploadOk();
    ulogf("OK HTTP %d %u bytes total=%lums cap=%lu http=%lu", code, (unsigned)jpegLen, tDone - t0, tCap - t0,
          tHttp - tHttp0);
  } else {
    if (code >= 0) {
      snprintf(uploadStatus, sizeof(uploadStatus), "Last upload failed (HTTP %d)", code);
    } else {
      strncpy(uploadStatus, "Last upload failed, no connection", sizeof(uploadStatus));
    }
    kerryHealthNoteUploadFail(uploadStatus);
    ulogw("failed HTTP %d total=%lums cap=%lu http=%lu", code, tDone - t0, tCap - t0, tHttp - tHttp0);
  }
  return ok;
}

static esp_err_t redirectIndex(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/index.html");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

static void logHttp(httpd_req_t *req) {
  if (!kerry_log_http_verbose(req->uri)) return;
  const char *method = "OTHER";
  if (req->method == HTTP_GET) method = "GET";
  else if (req->method == HTTP_POST) method = "POST";
  KERRY_LOGI("http", "%s %s", method, req->uri);
}

static String readRequestBody(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 700) return "";
  String body;
  body.reserve(req->content_len);
  int left = req->content_len;
  char buf[128];
  while (left > 0) {
    int n = httpd_req_recv(req, buf, left > (int)sizeof(buf) ? sizeof(buf) : left);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) body += buf[i];
    left -= n;
  }
  return body;
}

static esp_err_t imageHandler(httpd_req_t *req) {
  logHttp(req);
  String body = readRequestBody(req);
  if (!body.length()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty form");
    return ESP_FAIL;
  }

  const char *err = kerryImageSet(formInt(body, "framesz", camFrameSize), formInt(body, "bright", camBrightness),
                                  formInt(body, "contr", camContrast), formInt(body, "sat", camSaturation), -1, -1);
  if (err) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
    return ESP_FAIL;
  }
  return redirectIndex(req);
}

bool kerryUploadNow(char *message, size_t messageLen) {
  if (message && messageLen) message[0] = '\0';
  if (streamActive || livePushActive) {
    if (message && messageLen) strncpy(message, "Close the live stream first", messageLen);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED || !portalOk) {
    if (message && messageLen) strncpy(message, "Router Wi-Fi and internet required", messageLen);
    return false;
  }
  if (uploadInProgress || uploadNowRequested) {
    if (message && messageLen) strncpy(message, "Upload already in progress", messageLen);
    return false;
  }
  uploadNowRequested = true;
  if (message && messageLen) strncpy(message, "Upload queued", messageLen);
  return true;
}

void kerryRetryPortal() {
  portalOk = false;
  crewAuthenticated = false;
  portalAuthFailCycles = 0;
  portalFullCycles = 0;
  sessionId[0] = '\0';
  nextPortalMs = millis();
}

static esp_err_t uploadNowHandler(httpd_req_t *req) {
  logHttp(req);
  char msg[96];
  if (!kerryUploadNow(msg, sizeof msg)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL;
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/index.html");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

static bool jpegHasEoi(const uint8_t *buf, size_t len) {
  if (!buf || len < 4 || buf[0] != 0xff || buf[1] != 0xd8) return false;
  return buf[len - 2] == 0xff && buf[len - 1] == 0xd9;
}

static esp_err_t captureHandler(httpd_req_t *req) {
  logHttp(req);
  if (!lockCam(2000)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera is busy");
    return ESP_FAIL;
  }
  if (!cameraReady) {
    unlockCam();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera is not ready");
    return ESP_FAIL;
  }
  if (ov5640SketchActive) {
    camera_fb_t *warm = camFbGetHeldLock(false);
    if (warm) esp_camera_fb_return(warm);
  }
  dropHeldStaleFrame();
  camera_fb_t *fb = nullptr;
  for (int attempt = 0; attempt < 4; attempt++) {
    fb = camFbGetHeldLock(true);
    if (!fb) break;
    if (jpegHasEoi(fb->buf, fb->len)) break;
    logw("capture: truncated JPEG (%u bytes, %ux%u), retry %d", (unsigned)fb->len, fb->width, fb->height,
         attempt + 1);
    esp_camera_fb_return(fb);
    fb = nullptr;
    delay(80);
  }
  if (!fb) {
    unlockCam();
    serviceExposureRevertIfNeeded();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
    return ESP_FAIL;
  }
  logDeliveredFrameAge("capture", fb);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

  size_t jpegLen = fb->len;
  const uint16_t capW = fb->width;
  const uint16_t capH = fb->height;
  uint8_t *jpegCopy = allocUploadBuffer(jpegLen);
  if (!jpegCopy) {
    esp_camera_fb_return(fb);
    unlockCam();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  memcpy(jpegCopy, fb->buf, jpegLen);
  esp_camera_fb_return(fb);
  unlockCam();
  serviceExposureRevertIfNeeded();

  uint8_t *overlayBuf = nullptr;
  size_t overlayLen = 0;
  const char *sendBuf = (const char *)jpegCopy;
  size_t sendLen = jpegLen;
  if (tryOverlay("capture", jpegCopy, jpegLen, &overlayBuf, &overlayLen)) {
    sendBuf = (const char *)overlayBuf;
    sendLen = overlayLen;
  }

  esp_err_t res = httpd_resp_send(req, sendBuf, sendLen);
  if (overlayBuf) free(overlayBuf);
  free(jpegCopy);
  if (!jpegHasEoi((const uint8_t *)sendBuf, sendLen)) {
    logw("capture sent %u bytes without JPEG EOI (%ux%u)", (unsigned)sendLen, capW, capH);
  } else {
    logf("capture %u bytes %ux%u", (unsigned)sendLen, capW, capH);
  }
  return res;
}

static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static bool jsonFlag(const String &body, const char *key) {
  String needle = String("\"") + key + "\":";
  int i = body.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
  return body.startsWith("true", i);
}

static int jsonNumber(const String &body, const char *key, int fallback) {
  String needle = String("\"") + key + "\":";
  int i = body.indexOf(needle);
  if (i < 0) return fallback;
  return body.substring(i + needle.length()).toInt();
}

static bool liveAsk(int *secondsOut, int *intervalOut) {
  *secondsOut = 0;
  *intervalOut = 500;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(8000);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, LIVE_WANT_URL)) return false;
  http.addHeader("X-API-Key", UPLOAD_API_KEY);
  int code = http.GET();
  String body = code > 0 ? http.getString() : "";
  http.end();
  if (code != 200 || !jsonFlag(body, "stream")) return false;
  int sec = jsonNumber(body, "seconds", 0);
  int gap = jsonNumber(body, "interval_ms", 500);
  if (sec < 1) return false;
  if (sec > 30) sec = 30;
  if (gap < 400) gap = 400;
  if (gap > 5000) gap = 5000;
  *secondsOut = sec;
  *intervalOut = gap;
  return true;
}

static bool livePostFrame(const uint8_t *jpeg, size_t len, bool *stop) {
  *stop = true;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(8000);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  if (!http.begin(client, LIVE_FRAME_URL)) return false;
  http.addHeader("X-API-Key", UPLOAD_API_KEY);
  http.addHeader("Content-Type", "image/jpeg");
  int code = http.POST(const_cast<uint8_t *>(jpeg), len);
  String body = code > 0 ? http.getString() : "";
  http.end();
  *stop = jsonFlag(body, "stop") || code == 409 || code == 401;
  return code >= 200 && code < 300 && !*stop;
}

static void pushLiveBurst(int seconds, int intervalMs) {
  if (seconds < 1 || streamActive || otaInProgress || uploadInProgress) return;
  if (seconds > 30) seconds = 30;
  livePushActive = true;
  kerryWdtLenient();
  uint32_t deadline = millis() + (uint32_t)seconds * 1000UL;
  uint32_t hardStop = millis() + LIVE_MAX_MS;
  if ((int32_t)(deadline - hardStop) > 0) deadline = hardStop;
  ulogf("live push %ds every %dms", seconds, intervalMs);
  int sent = 0;
  while ((int32_t)(millis() - deadline) < 0 && !streamActive && !otaInProgress) {
    uint32_t t0 = millis();
    if (!lockCam(2000)) break;
    if (!cameraReady || streamActive) {
      unlockCam();
      break;
    }
    camera_fb_t *fb = camFbGetHeldLock(true);
    if (!fb) {
      unlockCam();
      delay(40);
      continue;
    }
    size_t jpegLen = fb->len;
    uint8_t *jpeg = allocUploadBuffer(jpegLen);
    if (!jpeg) {
      esp_camera_fb_return(fb);
      unlockCam();
      ulogw("live push out of memory");
      break;
    }
    memcpy(jpeg, fb->buf, jpegLen);
    esp_camera_fb_return(fb);
    unlockCam();
    bool stop = false;
    bool ok = livePostFrame(jpeg, jpegLen, &stop);
    free(jpeg);
    if (!ok || stop) break;
    sent++;
    uint32_t spent = millis() - t0;
    if (spent < (uint32_t)intervalMs) delay((uint32_t)intervalMs - spent);
  }
  ulogf("live push done, %d frames", sent);
  livePushActive = false;
  kerryWdtRestore();
}

static void serviceLive() {
  if (livePushActive || streamActive || otaInProgress || uploadInProgress || kerryEcoNetBusy()) return;
  if (!portalOk || WiFi.status() != WL_CONNECTED) return;
  static uint32_t nextLivePollMs = 0;
  if (nextLivePollMs != 0 && (int32_t)(millis() - nextLivePollMs) < 0) return;
  nextLivePollMs = millis() + LIVE_POLL_MS;
  int seconds = 0;
  int interval = 500;
  disableLoopWDT();
  bool go = liveAsk(&seconds, &interval);
  enableLoopWDT();
  if (!go) return;
  pushLiveBurst(seconds, interval);
  nextLivePollMs = millis() + LIVE_POLL_MS;
}

static esp_err_t streamHandler(httpd_req_t *req) {
  logHttp(req);
  if (livePushActive) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Remote live view in progress");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, STREAM_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  streamActive = true;
  char part[80];
  int misses = 0;
  while (true) {
    if (!lockCam(1000)) break;
    if (!cameraReady) {
      unlockCam();
      break;
    }
    camera_fb_t *fb = camFbGetHeldLock(true);
    if (!fb) {
      unlockCam();
      if (++misses > 20) break;
      delay(20);
      continue;
    }
    misses = 0;
    size_t len = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    esp_err_t res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    unlockCam();
    if (res != ESP_OK) break;
  }
  streamActive = false;
  return ESP_OK;
}

static esp_err_t flashHandler(httpd_req_t *req) {
  logHttp(req);
  flashOn = !flashOn;
  if (flashOn) flashOnSince = millis();
  applyFlash();
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/index.html");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

static esp_err_t flipHHandler(httpd_req_t *req) {
  logHttp(req);
  flipH = !flipH;
  saveImageSettings();
  if (lockCam(2000)) {
    applyCameraFlip();
    unlockCam();
  }
  logf("horizontal flip %s", flipH ? "on" : "off");
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/index.html");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

static esp_err_t flipVHandler(httpd_req_t *req) {
  logHttp(req);
  flipV = !flipV;
  saveImageSettings();
  if (lockCam(2000)) {
    applyCameraFlip();
    unlockCam();
  }
  logf("vertical flip %s", flipV ? "on" : "off");
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/index.html");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

static esp_err_t retryHandler(httpd_req_t *req) {
  logHttp(req);
  kerryRetryPortal();
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/index.html");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

static bool updateKeyOk(httpd_req_t *req) {
  char key[32];
  if (httpd_req_get_hdr_value_str(req, "X-Update-Key", key, sizeof(key)) != ESP_OK) return false;
  return strcmp(key, AP_PASS) == 0;
}

static esp_err_t updateInfoHandler(httpd_req_t *req) {
  logHttp(req);
  char body[640];
  snprintf(body, sizeof(body),
           "KerryCam firmware %s (env=%s git=%s)\n\n"
           "POST firmware.bin to this URL.\n"
           "Headers: Content-Type: application/octet-stream, X-Update-Key: <AP password>\n"
           "Close the live stream first.\n"
           "Example:\n"
           "curl -X POST --data-binary @firmware.bin \\\n"
           "  -H \"Content-Type: application/octet-stream\" \\\n"
           "  -H \"X-Update-Key: YOUR_AP_PASSWORD\" \\\n"
           "  http://192.168.4.1/update\n",
           kerryFirmwareLabel(), kerryFirmwareEnv(), kerryFirmwareGit());
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t updateHandler(httpd_req_t *req) {
  logHttp(req);
  if (!updateKeyOk(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Missing or bad X-Update-Key header");
    return ESP_FAIL;
  }
  if (streamActive) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Close the live stream before updating");
    return ESP_FAIL;
  }
  if (camGrabActive) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Wait for the current capture to finish");
    return ESP_FAIL;
  }
  if (req->content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length required");
    return ESP_FAIL;
  }

  logf("OTA upload starting, %d bytes", req->content_len);
  otaInProgress = true;
  nextPortalMs = millis() + 600000;

  if (lockCam(5000)) {
    if (cameraReady) {
      esp_camera_deinit();
      cameraReady = false;
    }
    unlockCam();
  }

  if (!Update.begin(req->content_len)) {
    otaInProgress = false;
    logf("OTA begin failed: %s", Update.errorString());
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, Update.errorString());
    return ESP_FAIL;
  }

  uint8_t buf[4096];
  int remaining = req->content_len;
  size_t writtenTotal = 0;
  while (remaining > 0) {
    int chunk = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
    int received = httpd_req_recv(req, (char *)buf, chunk);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (received <= 0) {
      Update.abort();
      otaInProgress = false;
      logf("OTA upload interrupted while receiving");
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
      return ESP_FAIL;
    }
    size_t written = Update.write(buf, received);
    if (written != (size_t)received) {
      Update.abort();
      otaInProgress = false;
      logf("OTA write failed: %s", Update.errorString());
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, Update.errorString());
      return ESP_FAIL;
    }
    remaining -= received;
    writtenTotal += written;
  }

  if (!Update.end(true)) {
    otaInProgress = false;
    logf("OTA finalize failed: %s", Update.errorString());
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, Update.errorString());
    return ESP_FAIL;
  }

  logf("OTA complete, %u bytes, rebooting", (unsigned)writtenTotal);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "OK, rebooting into new firmware");
  delay(500);
  ESP.restart();
  return ESP_OK;
}

static esp_err_t redirectHome(httpd_req_t *req, httpd_err_code_t err) {
  (void)err;
  logf("HTTP captive redirect %s", req->uri);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/login.html");
  httpd_resp_send(req, "Open the camera setup page", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static void registerUri(httpd_handle_t server, httpd_uri_t *uri) {
  esp_err_t err = httpd_register_uri_handler(server, uri);
  if (err != ESP_OK) logw("HTTP route failed %s (%s)", uri->uri, esp_err_to_name(err));
}

static int apChannel();

static void logHeartbeat() {
  char snap[176];
  const char *ship =
      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : (wifiSsid.length() ? "joining" : "off");
  snprintf(snap, sizeof snap, "ch=%d ship=%s portal=%s ap_clients=%u cam=%d status=%s", apChannel(), ship,
           portalOk ? "open" : "closed", WiFi.softAPgetStationNum(), cameraReady ? 1 : 0, portalStatus);
  if (kerry_log_if_changed("heartbeat", snap)) {
    logf("%s", snap);
    return;
  }
  if (kerry_log_every_ms("heartbeat", 120000)) logd("%s", snap);
}

static void startHttp() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 48;
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;
  config.backlog_conn = 5;
  config.stack_size = 16384;
  config.recv_wait_timeout = 60;
  config.send_wait_timeout = 30;
  config.server_port = 80;
  if (httpd_start(&httpd, &config) != ESP_OK) {
    Serial.println("Web server failed to start");
    return;
  }

  webServerRegister(httpd);
  httpd_uri_t image = {.uri = "/image", .method = HTTP_POST, .handler = imageHandler, .user_ctx = nullptr};
  httpd_uri_t capture = {.uri = "/capture", .method = HTTP_GET, .handler = captureHandler, .user_ctx = nullptr};
  httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr};
  httpd_uri_t flash = {.uri = "/flash", .method = HTTP_GET, .handler = flashHandler, .user_ctx = nullptr};
  httpd_uri_t flipH = {.uri = "/flip/h", .method = HTTP_GET, .handler = flipHHandler, .user_ctx = nullptr};
  httpd_uri_t flipV = {.uri = "/flip/v", .method = HTTP_GET, .handler = flipVHandler, .user_ctx = nullptr};
  httpd_uri_t retry = {.uri = "/retry", .method = HTTP_GET, .handler = retryHandler, .user_ctx = nullptr};
  httpd_uri_t updateInfo = {.uri = "/update", .method = HTTP_GET, .handler = updateInfoHandler, .user_ctx = nullptr};
  httpd_uri_t updatePost = {.uri = "/update", .method = HTTP_POST, .handler = updateHandler, .user_ctx = nullptr};
  httpd_uri_t uploadNow = {.uri = "/upload/now", .method = HTTP_GET, .handler = uploadNowHandler, .user_ctx = nullptr};
  registerUri(httpd, &image);
  registerUri(httpd, &capture);
  registerUri(httpd, &stream);
  registerUri(httpd, &flash);
  registerUri(httpd, &flipH);
  registerUri(httpd, &flipV);
  registerUri(httpd, &retry);
  registerUri(httpd, &updateInfo);
  registerUri(httpd, &updatePost);
  registerUri(httpd, &uploadNow);
  httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, redirectHome);
}

static int apChannel() {
  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  if (esp_wifi_get_config(WIFI_IF_AP, &conf) != ESP_OK || conf.ap.ssid_len == 0) return 0;
  return conf.ap.channel;
}

static void startApOnChannel(int channel);

static bool wifiBusyBlockingReconnect() { return otaInProgress || uploadInProgress || kerryEcoNetBusy(); }

static uint32_t wifiReconnectDelayMs() {
  unsigned shift = wifiReconnectFails > 4 ? 4 : wifiReconnectFails;
  uint32_t delayMs = WIFI_RECONNECT_MIN_MS << shift;
  if (delayMs > WIFI_RECONNECT_MAX_MS) delayMs = WIFI_RECONNECT_MAX_MS;
  return delayMs;
}

static void scheduleWifiReconnect(uint32_t delayMs) {
  uint32_t when = millis() + delayMs;
  if (!nextWifiReconnectMs || (int32_t)(when - nextWifiReconnectMs) > 0) nextWifiReconnectMs = when;
}

static void beginShipWifi(const char *why) {
  if (!wifiSsid.length()) return;
  logf("ship Wi-Fi join (%s) ssid='%s'", why, wifiSsid.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

static void ensureStaMode() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  if (mode != WIFI_MODE_APSTA) WiFi.mode(WIFI_AP_STA);
}

static void markShipWifiDown() {
  portalOk = false;
  crewAuthenticated = false;
  wifiStaHasIp = false;
}

static bool scanShipBssid(uint8_t bssid[6], uint8_t *channel, int *rssi) {
  wifi_scan_config_t sc = {};
  sc.ssid = (uint8_t *)wifiSsid.c_str();
  sc.show_hidden = true;
  sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  sc.scan_time.active.min = 100;
  sc.scan_time.active.max = 300;

  esp_wifi_scan_stop();
  esp_err_t err = esp_wifi_scan_start(&sc, true);
  if (err != ESP_OK) {
    logw("ship scan: %s", esp_err_to_name(err));
    return false;
  }

  uint16_t total = 0;
  esp_wifi_scan_get_ap_num(&total);
  if (!total) return false;
  if (total > 8) total = 8;
  wifi_ap_record_t *aps = (wifi_ap_record_t *)malloc(total * sizeof(wifi_ap_record_t));
  if (!aps) return false;

  uint16_t n = total;
  bool found = false;
  if (esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
    int best = -127;
    for (uint16_t i = 0; i < n; i++) {
      if (strcmp((const char *)aps[i].ssid, wifiSsid.c_str()) != 0) continue;
      if (!found || aps[i].rssi > best) {
        best = aps[i].rssi;
        memcpy(bssid, aps[i].bssid, 6);
        *channel = aps[i].primary;
        *rssi = best;
        found = true;
      }
    }
  }
  free(aps);
  return found;
}

static void softReconnectShipWifi(const char *why) {
  if (!wifiSsid.length()) return;
  if (wifiReconnectFails < 65535) wifiReconnectFails++;
  markShipWifiDown();
  logf("ship Wi-Fi soft reconnect (%s) attempt %u", why, wifiReconnectFails);
  setStatus("Reconnecting to router Wi-Fi");
  ensureStaMode();
  esp_err_t err = esp_wifi_connect();
  if (err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_SSID) {
    logw("esp_wifi_connect: %s", esp_err_to_name(err));
    beginShipWifi(why);
  } else if (err != ESP_OK) {
    logw("esp_wifi_connect: %s", esp_err_to_name(err));
  }
  scheduleWifiReconnect(wifiReconnectDelayMs());
}

static void fullReconnectShipWifi(const char *why) {
  if (!wifiSsid.length()) return;
  if (wifiReconnectFails < 65535) wifiReconnectFails++;
  markShipWifiDown();
  logf("ship Wi-Fi scan reconnect (%s) attempt %u", why, wifiReconnectFails);
  setStatus("Reconnecting to router Wi-Fi");

  disableLoopWDT();
  wifiManualDisconnect = true;
  WiFi.disconnect(false, false);
  delay(200);
  ensureStaMode();

  uint8_t bssid[6] = {};
  uint8_t channel = 0;
  int rssi = 0;
  if (scanShipBssid(bssid, &channel, &rssi) && channel >= 1 && channel <= 13) {
    logf("ship AP %02X:%02X:%02X:%02X:%02X:%02X ch %u rssi %d", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
         bssid[5], channel, rssi);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str(), channel, bssid);
  } else {
    logw("ship scan missed '%s', joining without BSSID", wifiSsid.c_str());
    beginShipWifi(why);
  }
  wifiManualDisconnect = false;
  enableLoopWDT();
  scheduleWifiReconnect(wifiReconnectDelayMs());
}

static void serviceWifiReconnect() {
  if (!wifiSsid.length()) return;

  wl_status_t sta = WiFi.status();
  if (wifiStaHasIp && sta == WL_CONNECTED) {
    if (wifiReconnectFails) {
      logf("ship Wi-Fi back online after %u reconnect attempt(s)", wifiReconnectFails);
      wifiReconnectFails = 0;
    }
    nextWifiReconnectMs = 0;
    wifiDownSinceMs = 0;
    wifiNoIpSinceMs = 0;
    nextNetRebootMs = 0;
    return;
  }

  if (wifiDownSinceMs == 0) wifiDownSinceMs = millis();
  if ((int32_t)(millis() - wifiDownSinceMs) >= (int32_t)WIFI_DOWN_REBOOT_MS) maybeNetReboot("ship Wi-Fi down");

  // STA_CONNECTED sets IDLE until DHCP finishes. Another join here drops the attempt.
  if (sta == WL_IDLE_STATUS && wifiReconnectFails > 0) {
    if (wifiNoIpSinceMs == 0) wifiNoIpSinceMs = millis();
    if ((int32_t)(millis() - wifiNoIpSinceMs) < (int32_t)WIFI_DHCP_WAIT_MS) return;
    wifiNoIpSinceMs = 0;
    fullReconnectShipWifi("noip");
    return;
  }
  wifiNoIpSinceMs = 0;

  if (nextWifiReconnectMs != 0 && (int32_t)(millis() - nextWifiReconnectMs) < 0) return;
  if (wifiBusyBlockingReconnect()) {
    scheduleWifiReconnect(5000);
    return;
  }

  if (wifiReconnectFails == 0 && !wifiStaWasConnected) {
    beginShipWifi("boot");
    wifiReconnectFails = 1;
    scheduleWifiReconnect(wifiReconnectDelayMs());
    return;
  }

  const char *why = wifiStaWasConnected ? "drop" : "retry";
  if (wifiReconnectFails >= WIFI_FULL_JOIN_EVERY && (wifiReconnectFails % WIFI_FULL_JOIN_EVERY) == 0) {
    fullReconnectShipWifi(why);
  } else {
    softReconnectShipWifi(why);
  }
}

static void startApOnChannel(int channel) {
  if (channel < 1 || channel > 13) channel = 1;
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, channel, 0, 4);
  logf("KerryCam %s on channel %d at %s", ok ? "up" : "FAILED", channel, WiFi.softAPIP().toString().c_str());
}

static void ensureAp() {
  if (WiFi.softAPIP() == AP_IP) return;
  int channel = 1;
  if (WiFi.status() == WL_CONNECTED) {
    int staChannel = WiFi.channel();
    if (staChannel >= 1 && staChannel <= 13) channel = staChannel;
  }
  logf("KerryCam access point missing, bringing it back");
  startApOnChannel(channel);
}

static void alignApToShipChannel(int radio) {
  if (radio < 1 || radio > 13) return;
  if (apChannel() == radio) return;
  logf("KerryCam is on channel %d but Crew is on channel %d, retuning once", apChannel(), radio);
  startApOnChannel(radio);
}

static const char *wifiReasonName(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
      return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
      return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:
      return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_LEAVE:
      return "ASSOC_LEAVE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return "4WAY_TIMEOUT";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
      return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
      return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
      return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
      return "CONNECTION_FAIL";
    default:
      return "OTHER";
  }
}

static void noteShipRadioDown(const char *status) {
  portalOk = false;
  crewAuthenticated = false;
  wifiStaHasIp = false;
  wifiStaWasConnected = true;
  if (wifiDownSinceMs == 0) wifiDownSinceMs = millis();
  setStatus(status);
  scheduleWifiReconnect(wifiReconnectDelayMs());
}

static void onWifi(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_START) {
    logd("ship Wi-Fi radio started");
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    logd("associated to the ship Wi-Fi");
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    bool fresh = !wifiStaHasIp;
    wifiReconnectFails = 0;
    nextWifiReconnectMs = 0;
    wifiStaWasConnected = true;
    wifiStaHasIp = true;
    wifiDownSinceMs = 0;
    wifiNoIpSinceMs = 0;
    nextNetRebootMs = 0;
    int radio = WiFi.channel();
    logf("ship IP %s on channel %d rssi %d", WiFi.localIP().toString().c_str(), radio, WiFi.RSSI());
    alignApToShipChannel(radio);
    kerryTimeOnWifiUp();
    if (fresh) {
      portalOk = false;
      crewAuthenticated = false;
      nextPortalMs = millis() + 1000;
      setStatus("Router Wi-Fi connected, checking internet");
    }
  } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
    if (wifiManualDisconnect) return;
    logf("ship Wi-Fi lost IP");
    noteShipRadioDown("Router Wi-Fi lost IP, reconnecting");
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    if (wifiManualDisconnect) {
      wifiStaHasIp = false;
      portalOk = false;
      crewAuthenticated = false;
      return;
    }
    uint8_t reason = info.wifi_sta_disconnected.reason;
    logf("ship Wi-Fi dropped, reason %u (%s)", reason, wifiReasonName(reason));
    noteShipRadioDown("Router Wi-Fi lost, reconnecting");
  } else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    logd("a device joined KerryCam (%u clients)", WiFi.softAPgetStationNum());
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    logd("a device left KerryCam (%u clients)", WiFi.softAPgetStationNum());
  } else if (event == ARDUINO_EVENT_WIFI_AP_START) {
    logd("KerryCam access point is up");
  }
}

static void *tlsCalloc(size_t n, size_t size) {
  if (n != 0 && size > (size_t)-1 / n) return nullptr;
  size_t bytes = n * size;
  void *p = nullptr;
  if (bytes >= 4096) p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
  return p;
}

void setup() {
  setApSsidFromMac();
  esp_brownout_disable();
  mbedtls_platform_set_calloc_free(tlsCalloc, heap_caps_free);
  applyFlash();
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Kerry Cam boot");
  Serial.printf("Firmware %s env=%s git=%s\n", kerryFirmwareLabel(), kerryFirmwareEnv(), kerryFirmwareGit());
  Serial.println("Serial log is 115200 baud (monitor DTR/RTS must be off)");
  Serial.printf("Join Wi-Fi %s / %s and open http://192.168.4.1/\n", AP_SSID, AP_PASS);
  Serial.printf("WebSocket log: ws://192.168.4.1:%u%s (or ship IP on port %u)\n", KERRY_WS_LOG_PORT,
                KERRY_WS_LOG_PATH, KERRY_WS_LOG_PORT);
  Serial.flush();

  kerry_ws_log_hook();
  logf("Firmware %s env=%s git=%s", kerryFirmwareLabel(), kerryFirmwareEnv(), kerryFirmwareGit());
  applyFlash();
  if (CAM_EN_GPIO >= 0) {
    pinMode(CAM_EN_GPIO, OUTPUT);
    digitalWrite(CAM_EN_GPIO, HIGH);
  }
  if (CAM_POWER_GPIO >= 0) {
    pinMode(CAM_POWER_GPIO, OUTPUT);
    digitalWrite(CAM_POWER_GPIO, HIGH);
  }
  camLock = xSemaphoreCreateMutex();
  loadConfig();
  kerryHealthInit();
  kerryTimeInit();
  kerryAwbLoad();
  kerryCamExposureLoad();
  kerry_overlay_cfg_load();
  kerryGeoPulseLoadConfig();
  ecowittInit();
  logf("saved ship Wi-Fi '%s', portal user '%s'",
       wifiSsid.length() ? wifiSsid.c_str() : "(none)",
       portalUser.length() ? portalUser.c_str() : "(none)");

  // Focus before the radio starts. WiFi during the AF download leaves the lens parked.
  if (!startCamera()) setStatus("Camera failed to start");

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setHostname("kerry-cam");
  WiFi.onEvent(onWifi);
  startApOnChannel(1);
  delay(300);
  kerry_ws_log_start();
  if (wifiSsid.length()) {
    scheduleWifiReconnect(500);
  }

  if (cameraReady) setStatus(wifiSsid.length() ? "Starting ship Wi-Fi" : "Enter the ship Wi-Fi below");
  startHttp();
  logf("web server listening on port 80");
  if (uploadIntervalSec) {
    nextUploadMs = millis() + uploadIntervalSec * 1000UL;
    ulogf("interval set to %lu s -> upload server", (unsigned long)uploadIntervalSec);
  }
}

void loop() {
  static uint32_t nextBeat = 0;
  if (flashOn && millis() - flashOnSince > FLASH_MAX_MS) {
    flashOn = false;
    applyFlash();
    logf("flash turned off");
  }
  if (millis() >= nextBeat) {
    nextBeat = millis() + 5000;
    ensureAp();
    logHeartbeat();
  }
  serviceWifiReconnect();
  kerryTimeLoop();
  static uint32_t nextCamRetryMs = 0;
  if (!cameraReady && !otaInProgress && !streamActive && !uploadInProgress && !kerryEcoNetBusy()) {
    if (nextCamRetryMs == 0) nextCamRetryMs = millis() + 3000;
    if ((int32_t)(millis() - nextCamRetryMs) >= 0) {
      nextCamRetryMs = millis() + 15000;
      if (lockCam(500)) {
        logw("camera down, retrying init");
        disableLoopWDT();
        startCamera();
        enableLoopWDT();
        unlockCam();
      }
    }
  } else {
    nextCamRetryMs = 0;
  }
  static bool portalWasOk = false;
  if (portalOk && !portalWasOk && kerry_overlay_enabled() && kerryGeoPulseHasCredentials() &&
      WiFi.status() == WL_CONNECTED && !streamActive && !otaInProgress && !uploadInProgress && !kerryEcoNetBusy()) {
    logf("GeoPulse prefetch (internet just opened)");
    disableLoopWDT();
    if (kerryGeoPulseEnsureNav(true)) {
      logd("GeoPulse prefetch OK (%d ms)", kerryGeoPulseLastNav()->fetch_ms);
    } else {
      logw("GeoPulse prefetch failed: %s", kerryGeoPulseLastError());
    }
    enableLoopWDT();
  }
  portalWasOk = portalOk;
  servicePortal();
  if (kerry_overlay_enabled() && WiFi.status() == WL_CONNECTED && !streamActive && !otaInProgress &&
      !uploadInProgress && !kerryEcoNetBusy() && kerryGeoPulseHasCredentials() &&
      kerryGeoPulseCacheAgeMs() > 60000UL) {
    static uint32_t nextNavRefreshMs = 0;
    if (millis() >= nextNavRefreshMs) {
      nextNavRefreshMs = millis() + 60000UL;
      disableLoopWDT();
      bool navOk = kerryGeoPulseEnsureNav(false);
      enableLoopWDT();
      if (!navOk) {
        logw("GeoPulse nav refresh failed: %s", kerryGeoPulseLastError());
      } else if (kerry_log_every_ms("geopulse_refresh", 300000)) {
        logd("GeoPulse nav refreshed (%d ms)", kerryGeoPulseLastNav()->fetch_ms);
      }
    }
  }
  serviceLive();
  serviceExposureRevertIfNeeded();
  kerryCamTelemetryService(millis());
  if (uploadNowRequested && !streamActive && !livePushActive && !otaInProgress && kerryTryBeginUpload()) {
    uploadNowRequested = false;
    ulogf("manual upload");
    logUploadMem("start");
    kerryWdtLenient();
    uploadCaptureToFinestra();
    kerryWdtRestore();
    kerryEndUpload();
    if (uploadIntervalSec) nextUploadMs = millis() + uploadIntervalSec * 1000UL;
  }
  if (uploadIntervalSec > 0 && millis() >= nextUploadMs) {
    nextUploadMs = millis() + uploadIntervalSec * 1000UL;
    if (portalOk && WiFi.status() == WL_CONNECTED && !streamActive && !livePushActive && !otaInProgress &&
        kerryTryBeginUpload()) {
      ulogf("tick (every %lus)", (unsigned long)uploadIntervalSec);
      logUploadMem("start");
      kerryWdtLenient();
      uploadCaptureToFinestra();
      kerryWdtRestore();
      kerryEndUpload();
    } else {
      logUploadTickBlocked();
    }
  }
  delay(20);
}
