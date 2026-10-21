#include "overlay_layout.h"
#include "overlay_nav.h"

#include "kerry_log.h"

#include <Preferences.h>

#include <cstring>

bool g_ov_en = false;
ov_geom_t g_ov_geom;
static bool g_cfg_loaded = false;

static const char *EL_KEYS[OV_EL_N] = {"time", "lat", "lon", "spd", "crs", "dev", "mtc", "req"};

void kerry_overlay_cfg_load() {
  if (g_cfg_loaded) return;
  ov_geom_defaults(&g_ov_geom);
  Preferences prefs;
  prefs.begin("kerry", true);
  g_ov_en = prefs.getBool("ov_en", false);
  for (int i = 0; i < OV_EL_N; i++) {
    String p = String("ov_") + EL_KEYS[i];
    g_ov_geom.en[i] = prefs.getUChar((p + "_e").c_str(), g_ov_geom.en[i]);
    g_ov_geom.an[i] = prefs.getUChar((p + "_a").c_str(), g_ov_geom.an[i]);
    g_ov_geom.ox[i] = prefs.getShort((p + "_x").c_str(), g_ov_geom.ox[i]);
    g_ov_geom.oy[i] = prefs.getShort((p + "_y").c_str(), g_ov_geom.oy[i]);
  }
  prefs.end();
  g_cfg_loaded = true;
  KERRY_LOGI("overlay", "loaded enabled=%d", g_ov_en ? 1 : 0);
}

void kerry_overlay_cfg_save(bool enabled, const ov_geom_t *geom) {
  Preferences prefs;
  prefs.begin("kerry", false);
  g_ov_en = enabled;
  prefs.putBool("ov_en", enabled);
  if (geom) {
    g_ov_geom = *geom;
    for (int i = 0; i < OV_EL_N; i++) {
      String p = String("ov_") + EL_KEYS[i];
      prefs.putUChar((p + "_e").c_str(), geom->en[i]);
      prefs.putUChar((p + "_a").c_str(), geom->an[i]);
      prefs.putShort((p + "_x").c_str(), geom->ox[i]);
      prefs.putShort((p + "_y").c_str(), geom->oy[i]);
    }
  }
  prefs.end();
  g_cfg_loaded = true;
  KERRY_LOGI("overlay", "saved enabled=%d", enabled ? 1 : 0);
}

int kerry_overlay_el_index(const char *id) {
  if (!id) return -1;
  for (int i = 0; i < OV_EL_N; i++) {
    if (strcmp(id, EL_KEYS[i]) == 0) return i;
  }
  return -1;
}

const char *kerry_overlay_el_id(int index) {
  if (index < 0 || index >= OV_EL_N) return "";
  return EL_KEYS[index];
}
