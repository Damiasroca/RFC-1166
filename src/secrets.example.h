#pragma once
// Copy this file to secrets.h (same folder) and fill in your values.
// secrets.h is gitignored and required to build (see scripts/gen_build_info.py).

// Camera setup AP password (web UI login and OTA X-Update-Key).
static const char KERRY_AP_PASS[] = "change-me";

// Remote upload / live stream backend (Finestra or your own server).
static const char KERRY_UPLOAD_URL[] = "https://your-host.example/api/upload";
static const char KERRY_LIVE_WANT_URL[] = "https://your-host.example/api/live/want";
static const char KERRY_LIVE_FRAME_URL[] = "https://your-host.example/api/live/frame";
static const char KERRY_UPLOAD_API_KEY[] = "your-upload-api-key";

// Ecowitt device registration (stationtype / model / default display name).
static const char KERRY_ECOWITT_STATION_TYPE[] = "MyStation";
static const char KERRY_ECOWITT_MODEL[] = "CAM1";
static const char KERRY_ECOWITT_SUGGEST_NAME[] = "My-Camera";

// GeoPulse API (defaults used when NVS has no saved credentials yet).
static const char KERRY_GEOPULSE_API_BASE[] = "https://api.example.com";
static const char KERRY_GEOPULSE_DEFAULT_USER[] = "";
static const char KERRY_GEOPULSE_DEFAULT_PASSWORD[] = "";
static const char KERRY_GEOPULSE_DEFAULT_DEVICE[] = "my-device";
