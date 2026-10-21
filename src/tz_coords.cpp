#include "tz_lookup.h"

#include <cmath>
#include <cstring>

typedef struct {
  float lat;
  float lon;
  const char *iana;
} tz_coord_t;

static const tz_coord_t kZones[] = {
    {51.51f, -0.13f, "Europe/London"},
    {53.35f, -6.26f, "Europe/Dublin"},
    {38.72f, -9.14f, "Europe/Lisbon"},
    {40.42f, -3.70f, "Europe/Madrid"},
    {48.86f, 2.35f, "Europe/Paris"},
    {50.85f, 4.35f, "Europe/Brussels"},
    {52.37f, 4.90f, "Europe/Amsterdam"},
    {52.52f, 13.41f, "Europe/Berlin"},
    {48.21f, 16.37f, "Europe/Vienna"},
    {50.08f, 14.44f, "Europe/Prague"},
    {47.50f, 19.04f, "Europe/Budapest"},
    {52.23f, 21.01f, "Europe/Warsaw"},
    {55.68f, 12.57f, "Europe/Copenhagen"},
    {59.91f, 10.75f, "Europe/Oslo"},
    {59.33f, 18.07f, "Europe/Stockholm"},
    {60.17f, 24.94f, "Europe/Helsinki"},
    {56.95f, 24.11f, "Europe/Riga"},
    {59.44f, 24.75f, "Europe/Tallinn"},
    {54.69f, 25.28f, "Europe/Vilnius"},
    {41.90f, 12.50f, "Europe/Rome"},
    {46.95f, 7.45f, "Europe/Zurich"},
    {47.37f, 8.54f, "Europe/Zurich"},
    {45.46f, 9.19f, "Europe/Rome"},
    {37.98f, 23.73f, "Europe/Athens"},
    {41.01f, 28.98f, "Europe/Istanbul"},
    {64.15f, -21.94f, "Atlantic/Reykjavik"},
    {35.90f, 14.51f, "Europe/Malta"},
    {43.30f, 5.37f, "Europe/Paris"},
    {43.60f, 1.44f, "Europe/Paris"},
    {49.44f, -2.59f, "Europe/Guernsey"},
    {54.60f, -5.93f, "Europe/Belfast"},
    {57.15f, -2.10f, "Europe/London"},
    {58.97f, 5.73f, "Europe/Oslo"},
    {63.43f, 10.40f, "Europe/Oslo"},
    {65.68f, -18.09f, "Atlantic/Reykjavik"},
    {60.39f, 5.32f, "Europe/Oslo"},
    {62.47f, 6.15f, "Europe/Oslo"},
    {57.71f, 11.97f, "Europe/Stockholm"},
    {55.68f, 12.57f, "Europe/Copenhagen"},
    {54.35f, 18.65f, "Europe/Warsaw"},
    {54.52f, 18.53f, "Europe/Warsaw"},
    {53.55f, 9.99f, "Europe/Berlin"},
    {53.87f, 10.69f, "Europe/Berlin"},
    {54.09f, 12.14f, "Europe/Berlin"},
    {57.05f, 9.92f, "Europe/Copenhagen"},
    {56.16f, 10.20f, "Europe/Copenhagen"},
    {57.05f, 8.42f, "Europe/Copenhagen"},
    {62.00f, 6.77f, "Europe/Oslo"},
    {61.50f, 5.73f, "Europe/Oslo"},
    {58.38f, 26.72f, "Europe/Tallinn"},
    {59.00f, 5.73f, "Europe/Oslo"},
};

const char *kerry_tz_from_coords(double lat, double lon, char *ianaOut, size_t ianaLen) {
  if (!ianaOut || ianaLen < 2) return nullptr;
  ianaOut[0] = '\0';
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return nullptr;

  const tz_coord_t *best = &kZones[0];
  double bestD = 1e18;
  for (size_t i = 0; i < sizeof kZones / sizeof kZones[0]; i++) {
    double dlat = lat - kZones[i].lat;
    double dlon = lon - kZones[i].lon;
    double d = dlat * dlat + dlon * dlon;
    if (d < bestD) {
      bestD = d;
      best = &kZones[i];
    }
  }
  strncpy(ianaOut, best->iana, ianaLen - 1);
  ianaOut[ianaLen - 1] = '\0';
  return ianaOut;
}
