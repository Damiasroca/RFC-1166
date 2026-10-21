#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

#define KERRY_AWB_PRESET0 5
#define KERRY_AWB_PRESET_N 4
#define KERRY_AWB_PRESET_LAST (KERRY_AWB_PRESET0 + KERRY_AWB_PRESET_N - 1)
#define KERRY_AWB_NAME_MAX 15

typedef struct {
  uint8_t used;
  uint8_t r, g, b;
  char name[KERRY_AWB_NAME_MAX + 1];
} kerry_awb_preset_t;

void kerryAwbLoad();
void kerryAwbApply();
void kerryAwbFillJson(JsonDocument &doc);
const char *kerryAwbSet(int awb, int r, int g, int b);
const char *kerryAwbPresetSave(const char *name, int r, int g, int b);
const char *kerryAwbPresetDelete(int slot);
