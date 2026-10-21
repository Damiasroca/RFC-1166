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

uint8_t kerryOv2640DefaultClkrc() { return g_defClkrc; }

uint16_t kerryOv2640BaseFrameLines(framesize_t fs) {
  if ((int)fs <= (int)FRAMESIZE_CIF) return 336;
  if ((int)fs <= (int)FRAMESIZE_SVGA) return 672;
  return 1248;
}

void kerryOv2640PeriodScale(uint8_t userClkrc, uint32_t *num, uint32_t *den) {
  uint8_t applied = userClkrc == KERRY_EXP_CLKRC_SENTINEL ? g_defClkrc : userClkrc;
  uint32_t dMul = (g_defClkrc & 0x80) ? 2u : 1u;
  uint32_t uMul = (applied & 0x80) ? 2u : 1u;
  uint32_t dDiv = (uint32_t)(g_defClkrc & 0x3F) + 1u;
  uint32_t uDiv = (uint32_t)(applied & 0x3F) + 1u;
  *num = dMul * uDiv;
  *den = uMul * dDiv;
  if (*den == 0) *den = 1;
}

static uint32_t g_basePeriodMs[FRAMESIZE_INVALID];

uint32_t kerryOv2640BasePeriodMs(framesize_t fs) {
  if ((int)fs < 0 || fs >= FRAMESIZE_INVALID) return 0;
  return g_basePeriodMs[fs];
}

void kerryOv2640NoteBaseline(framesize_t fs, uint8_t clkrc, uint16_t dummyLines, bool aecOn, uint16_t aecValue,
                             uint32_t measuredMs) {
  if ((int)fs < 0 || fs >= FRAMESIZE_INVALID || measuredMs == 0) return;
  uint32_t base = kerryOv2640BaseFrameLines(fs);
  uint32_t lines = base + dummyLines;
  if (!aecOn && aecValue > lines) lines = aecValue;
  uint32_t num = 1;
  uint32_t den = 1;
  kerryOv2640PeriodScale(clkrc, &num, &den);
  if (num == 0 || lines == 0 || base == 0) return;
  uint64_t baseline = (uint64_t)measuredMs * den * base / ((uint64_t)num * lines);
  if (baseline == 0) baseline = 1;
  g_basePeriodMs[fs] = (uint32_t)baseline;
  KERRY_LOGD("exposure", "ov2640 baseline %ums framesize %d measured %ums", (unsigned)baseline, (int)fs,
             (unsigned)measuredMs);
}

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

int kerryOv2640SetAecValue(sensor_t *s, int value) {
  if (!s || !s->set_reg) return -1;
  if (value < 0) value = 0;
  if (value > 65535) value = 65535;
  if (s->set_reg(s, 0x0104, 0x03, value & 0x03) != 0) return -1;
  if (s->set_reg(s, 0x0110, 0xFF, (value >> 2) & 0xFF) != 0) return -1;
  if (s->set_reg(s, 0x0145, 0x3F, (value >> 10) & 0x3F) != 0) return -1;
  return 0;
}
