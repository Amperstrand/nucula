#pragma once
// Host shim: in-memory NVS (namespaced key -> value store) so the
// wallet persistence code links and runs on the host. NOT a behavioral
// clone of flash NVS: no wear, no partitions, no iteration order.
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

typedef void *nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v);

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v);

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);

#ifdef __cplusplus
}
#endif
