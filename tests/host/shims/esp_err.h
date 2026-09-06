#pragma once
// Host shim: ESP error codes used by the ported sources.
#include <cstdint>

typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_INVALID_STATE 0x103

static inline const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    default: return "ESP_ERR_UNKNOWN";
    }
}
