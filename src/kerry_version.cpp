#include "kerry_version.h"

#include <cstdio>

#include "build_info.h"
#include "version.h"

const char *kerryFirmwareVersion() { return KERRY_VERSION; }

const char *kerryFirmwareBuildId() { return KERRY_BUILD_ID; }

const char *kerryFirmwareEnv() { return KERRY_BUILD_ENV; }

const char *kerryFirmwareGit() { return KERRY_GIT_SHA; }

const char *kerryFirmwareLabel() {
  static char label[48];
  snprintf(label, sizeof label, "%s+%s", KERRY_VERSION, KERRY_BUILD_ID);
  return label;
}
