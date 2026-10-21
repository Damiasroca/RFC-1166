#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
  OV_EL_TIME = 0,
  OV_EL_LAT,
  OV_EL_LON,
  OV_EL_SPD,
  OV_EL_CRS,
  OV_EL_DEV,
  OV_EL_MTC,
  OV_EL_REQ,
  OV_EL_N
};

enum {
  OV_AN_TL = 0,
  OV_AN_TC,
  OV_AN_TR,
  OV_AN_CL,
  OV_AN_CR,
  OV_AN_BL,
  OV_AN_BC,
  OV_AN_BR
};

typedef struct {
  bool ok;
  int fetch_ms;
  char time_text[24];
  char lat[20];
  char lon[20];
  char speed[24];
  char course[12];
  char device_id[32];
  char match_type[20];
  char requested[32];
} ov_nav_t;

typedef struct {
  uint8_t en[OV_EL_N];
  uint8_t an[OV_EL_N];
  int16_t ox[OV_EL_N];
  int16_t oy[OV_EL_N];
} ov_geom_t;
