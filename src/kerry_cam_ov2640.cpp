#include "kerry_cam_ov2640.h"

#include "kerry_cam_exposure.h"

bool kerryOv2640Is(uint16_t pid) { return pid == OV2640_PID; }

int kerryOv2640ApplyTiming(sensor_t *s, uint8_t clkrc, uint16_t dummyLines) {
  if (!s || !s->set_reg) return 0;
  int ret = 0;
  if (clkrc != KERRY_EXP_CLKRC_SENTINEL) {
    ret = s->set_reg(s, 0x0111, 0xFF, clkrc);
    if (ret) return ret;
  }
  if (dummyLines > 0) {
    ret = s->set_reg(s, 0x012D, 0xFF, (int)(dummyLines & 0xFF));
    if (ret) return ret;
    ret = s->set_reg(s, 0x012E, 0xFF, (int)((dummyLines >> 8) & 0xFF));
  }
  return ret;
}
