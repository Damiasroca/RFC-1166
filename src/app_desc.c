#include "build_info.h"
#include "version.h"

#include <esp_app_desc.h>
#include <esp_idf_version.h>

#define KERRY_STR_HELPER(x) #x
#define KERRY_STR(x) KERRY_STR_HELPER(x)

#ifndef KERRY_APP_VERSION
#define KERRY_APP_VERSION KERRY_VERSION "+" KERRY_BUILD_ID
#endif

const __attribute__((section(".rodata_desc"))) esp_app_desc_t esp_app_desc = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD,
    .version = KERRY_APP_VERSION,
    .project_name = "KerryCam",
    .time = __TIME__,
    .date = __DATE__,
    .idf_ver = "v" KERRY_STR(ESP_IDF_VERSION_MAJOR) "." KERRY_STR(ESP_IDF_VERSION_MINOR) "." KERRY_STR(
        ESP_IDF_VERSION_PATCH),
};
