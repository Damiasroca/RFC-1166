#include "kerry_cam_ov2640.h"

#include "kerry_cam_exposure.h"
#include "kerry_log.h"

static bool g_haveDefaults = false;
static bool g_haveDummy = false;
static uint8_t g_defClkrc = 0;
static uint16_t g_defDummy = 0;

bool kerryOv2640Is(uint16_t pid) { return pid == OV2640_PID; }

static int readReg(sensor_t *s, int reg) {
  if (!s || !s->get_reg) return -1;
  return s->get_reg(s, reg, 0xFF);
}

void kerryOv2640ForgetDefaults() {
  g_haveDefaults = false;
  g_haveDummy = false;
}

bool kerryOv2640DefaultsReady() { return g_haveDefaults; }

const char *kerryOv2640CaptureDefaults(sensor_t *s) {
  if (!s || !s->get_reg) return "OV2640 register read is unavailable";
  int clk = readReg(s, 0x0111);
  int lo = readReg(s, 0x012D);
  int hi = readReg(s, 0x012E);
  if (clk < 0 || lo < 0 || hi < 0) return "OV2640 register read failed";
  g_defClkrc = (uint8_t)clk;
  // set_framesize rewrites CLKRC and leaves ADDVSL/ADDVSH alone, so dummy defaults
  // are taken only from the first read after init.
  if (!g_haveDummy) {
    g_defDummy = (uint16_t)(lo & 0xFF) | ((uint16_t)(hi & 0xFF) << 8);
    g_haveDummy = true;
  }
  g_haveDefaults = true;
  KERRY_LOGD("exposure", "ov2640 defaults clkrc=0x%02X dummy=%u", (unsigned)g_defClkrc, (unsigned)g_defDummy);
  return nullptr;
}

const char *kerryOv2640ApplyTiming(sensor_t *s, uint8_t clkrc, uint16_t dummyLines) {
  if (!g_haveDefaults || !g_haveDummy) return "OV2640 timing defaults were not captured";
  if (!s || !s->set_reg) return "Sensor rejected clock or dummy-line settings";
  uint8_t clk = clkrc == KERRY_EXP_CLKRC_SENTINEL ? g_defClkrc : clkrc;
  uint16_t dummy = dummyLines == 0 ? g_defDummy : dummyLines;
  if (s->set_reg(s, 0x0111, 0xFF, clk) != 0) return "Sensor rejected clock or dummy-line settings";
  if (s->set_reg(s, 0x012D, 0xFF, (int)(dummy & 0xFF)) != 0) return "Sensor rejected clock or dummy-line settings";
  if (s->set_reg(s, 0x012E, 0xFF, (int)((dummy >> 8) & 0xFF)) != 0)
    return "Sensor rejected clock or dummy-line settings";
  int gotClk = readReg(s, 0x0111);
  int gotLo = readReg(s, 0x012D);
  int gotHi = readReg(s, 0x012E);
  unsigned gotDummy = 0;
  if (gotLo >= 0 && gotHi >= 0) gotDummy = (unsigned)(gotLo & 0xFF) | ((unsigned)(gotHi & 0xFF) << 8);
  KERRY_LOGD("exposure", "ov2640 timing clkrc=0x%02X dummy=%u", gotClk < 0 ? 0 : (unsigned)gotClk, gotDummy);
  return nullptr;
}
