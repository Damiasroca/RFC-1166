#include "kerry_awb.h"

#include "kerry_log.h"
#include "kerry_shared.h"

#include <Preferences.h>
#include <esp_camera.h>

static uint8_t g_awb = 0;
static uint8_t g_awb_r = 0x5E;
static uint8_t g_awb_g = 0x41;
static uint8_t g_awb_b = 0x54;
static kerry_awb_preset_t g_preset[KERRY_AWB_PRESET_N];

static bool inRange(int v, int lo, int hi) { return v >= lo && v <= hi; }

static int presetSlot(int awb) {
  if (awb < KERRY_AWB_PRESET0 || awb > KERRY_AWB_PRESET_LAST) return -1;
  return awb - KERRY_AWB_PRESET0;
}

static const char *awbCheck(int awb, int r, int g, int b) {
  int slot = presetSlot(awb);
  if (awb < 0 || (awb > 4 && slot < 0)) return "awb mode must be 0-4 or a saved preset";
  if (slot >= 0 && !g_preset[slot].used) return "that preset is empty";
  if (!inRange(r, 0, 255) || !inRange(g, 0, 255) || !inRange(b, 0, 255)) return "gain must be 0-255";
  return nullptr;
}

static const char *copyName(char *dst, const char *src) {
  if (!src) return "Name the preset";
  while (*src == ' ') src++;
  size_t n = 0;
  while (src[n]) {
    unsigned char c = (unsigned char)src[n];
    if (c < 0x20) return "preset name has a bad character";
    if (n >= KERRY_AWB_NAME_MAX) return "preset name must be 1-15 characters";
    dst[n] = (char)c;
    n++;
  }
  while (n > 0 && dst[n - 1] == ' ') n--;
  dst[n] = 0;
  if (n == 0) return "Name the preset";
  return nullptr;
}

static void saveAwb() {
  Preferences prefs;
  prefs.begin("kerry", false);
  prefs.putUChar("awb", g_awb);
  prefs.putUChar("awb_r", g_awb_r);
  prefs.putUChar("awb_g", g_awb_g);
  prefs.putUChar("awb_b", g_awb_b);
  for (int i = 0; i < KERRY_AWB_PRESET_N; i++) {
    char key[8];
    snprintf(key, sizeof key, "awb_p%d", i);
    if (!g_preset[i].used) {
      prefs.remove(key);
      continue;
    }
    uint8_t blob[4 + KERRY_AWB_NAME_MAX + 1];
    blob[0] = g_preset[i].r;
    blob[1] = g_preset[i].g;
    blob[2] = g_preset[i].b;
    blob[3] = 1;
    strncpy((char *)&blob[4], g_preset[i].name, KERRY_AWB_NAME_MAX + 1);
    prefs.putBytes(key, blob, sizeof blob);
  }
  prefs.end();
}

void kerryAwbLoad() {
  Preferences prefs;
  prefs.begin("kerry", true);
  g_awb = prefs.getUChar("awb", 0);
  g_awb_r = prefs.getUChar("awb_r", 0x5E);
  g_awb_g = prefs.getUChar("awb_g", 0x41);
  g_awb_b = prefs.getUChar("awb_b", 0x54);
  memset(g_preset, 0, sizeof g_preset);
  for (int i = 0; i < KERRY_AWB_PRESET_N; i++) {
    char key[8];
    uint8_t blob[4 + KERRY_AWB_NAME_MAX + 1];
    snprintf(key, sizeof key, "awb_p%d", i);
    size_t n = prefs.getBytesLength(key);
    if (n < 4) continue;
    prefs.getBytes(key, blob, sizeof blob);
    if (!blob[3]) continue;
    g_preset[i].used = 1;
    g_preset[i].r = blob[0];
    g_preset[i].g = blob[1];
    g_preset[i].b = blob[2];
    strncpy(g_preset[i].name, (char *)&blob[4], KERRY_AWB_NAME_MAX);
    g_preset[i].name[KERRY_AWB_NAME_MAX] = 0;
  }
  prefs.end();
  int presets = 0;
  for (int i = 0; i < KERRY_AWB_PRESET_N; i++)
    if (g_preset[i].used) presets++;
  KERRY_LOGI("awb", "loaded mode=%u presets=%d", (unsigned)g_awb, presets);
}

void kerryAwbApply() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !s->set_whitebal || !s->set_awb_gain || !s->set_wb_mode) {
    KERRY_LOGW("awb", "apply skipped, sensor unavailable");
    return;
  }

  int mode = g_awb;
  if (mode == 0) {
    if (s->set_whitebal(s, 1) != 0 || s->set_awb_gain(s, 0) != 0 || s->set_wb_mode(s, 0) != 0) {
      KERRY_LOGW("awb", "apply failed for auto AWB");
    }
    return;
  }
  if (s->set_whitebal(s, 1) != 0 || s->set_awb_gain(s, 1) != 0) {
    KERRY_LOGW("awb", "apply failed enabling manual AWB");
    return;
  }
  if (mode >= KERRY_AWB_PRESET0 && mode <= KERRY_AWB_PRESET_LAST) {
    kerry_awb_preset_t *p = &g_preset[mode - KERRY_AWB_PRESET0];
    if (!p->used || !s->set_reg) return;
    if (isOV5640 || isOV3660) {
      uint16_t r = (uint16_t)p->r << 4;
      uint16_t g = (uint16_t)p->g << 4;
      uint16_t b = (uint16_t)p->b << 4;
      s->set_reg(s, 0x3406, 0x01, 0x01);
      s->set_reg(s, 0x3400, 0xFF, r >> 8);
      s->set_reg(s, 0x3401, 0xFF, r & 0xFF);
      s->set_reg(s, 0x3402, 0xFF, g >> 8);
      s->set_reg(s, 0x3403, 0xFF, g & 0xFF);
      s->set_reg(s, 0x3404, 0xFF, b >> 8);
      s->set_reg(s, 0x3405, 0xFF, b & 0xFF);
    } else {
      s->set_reg(s, 0x0C7, 0x40, 0x40);
      s->set_reg(s, 0x0CC, 0xFF, p->r);
      s->set_reg(s, 0x0CD, 0xFF, p->g);
      s->set_reg(s, 0x0CE, 0xFF, p->b);
    }
    return;
  }
  s->set_wb_mode(s, mode);
}

static bool appliedGains(int *r, int *g, int *b) {
  static const uint8_t modes[4][3] = {
      {0x5E, 0x41, 0x54},
      {0x65, 0x41, 0x4F},
      {0x52, 0x41, 0x66},
      {0x42, 0x3F, 0x71},
  };
  if (g_awb >= 1 && g_awb <= 4) {
    *r = modes[g_awb - 1][0];
    *g = modes[g_awb - 1][1];
    *b = modes[g_awb - 1][2];
    return true;
  }
  int slot = presetSlot(g_awb);
  if (slot >= 0 && g_preset[slot].used) {
    *r = g_preset[slot].r;
    *g = g_preset[slot].g;
    *b = g_preset[slot].b;
    return true;
  }
  return false;
}

void kerryAwbFillJson(JsonDocument &doc) {
  doc["awb"] = g_awb;
  doc["awb_r"] = g_awb_r;
  doc["awb_g"] = g_awb_g;
  doc["awb_b"] = g_awb_b;
  int r = 0, g = 0, b = 0;
  JsonArray live = doc["live"].to<JsonArray>();
  if (appliedGains(&r, &g, &b)) {
    live.add(r);
    live.add(g);
    live.add(b);
  }
  JsonArray presets = doc["presets"].to<JsonArray>();
  for (int i = 0; i < KERRY_AWB_PRESET_N; i++) {
    JsonObject row = presets.add<JsonObject>();
    row["used"] = g_preset[i].used != 0;
    row["name"] = g_preset[i].name;
    row["r"] = g_preset[i].r;
    row["g"] = g_preset[i].g;
    row["b"] = g_preset[i].b;
  }
}

const char *kerryAwbSet(int awb, int r, int g, int b) {
  const char *err = awbCheck(awb, r, g, b);
  if (err) return err;
  g_awb = (uint8_t)awb;
  g_awb_r = (uint8_t)r;
  g_awb_g = (uint8_t)g;
  g_awb_b = (uint8_t)b;
  saveAwb();
  kerryAwbApply();
  KERRY_LOGI("awb", "set mode=%d gains=%d/%d/%d", awb, r, g, b);
  return nullptr;
}

const char *kerryAwbPresetSave(const char *name, int r, int g, int b) {
  if (!inRange(r, 0, 255) || !inRange(g, 0, 255) || !inRange(b, 0, 255)) return "gain must be 0-255";
  char stored[KERRY_AWB_NAME_MAX + 1];
  const char *err = copyName(stored, name);
  if (err) return err;
  int slot = -1;
  for (int i = 0; i < KERRY_AWB_PRESET_N; i++) {
    if (!g_preset[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return "all preset slots are full";
  g_preset[slot].used = 1;
  g_preset[slot].r = (uint8_t)r;
  g_preset[slot].g = (uint8_t)g;
  g_preset[slot].b = (uint8_t)b;
  strncpy(g_preset[slot].name, stored, KERRY_AWB_NAME_MAX);
  g_preset[slot].name[KERRY_AWB_NAME_MAX] = 0;
  saveAwb();
  KERRY_LOGI("awb", "preset saved slot=%d name=%s %d/%d/%d", slot, stored, r, g, b);
  return nullptr;
}

const char *kerryAwbPresetDelete(int slot) {
  if (slot < 0 || slot >= KERRY_AWB_PRESET_N || !g_preset[slot].used) return "invalid preset slot";
  if (g_awb == KERRY_AWB_PRESET0 + slot) g_awb = 0;
  memset(&g_preset[slot], 0, sizeof g_preset[slot]);
  saveAwb();
  kerryAwbApply();
  KERRY_LOGI("awb", "preset deleted slot=%d", slot);
  return nullptr;
}
