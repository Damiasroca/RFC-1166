#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *kerry_tz_posix(const char *iana);
void kerry_tz_names_json(void *arr);

#ifdef __cplusplus
}
const char *kerry_tz_from_coords(double lat, double lon, char *ianaOut, size_t ianaLen);
#endif
