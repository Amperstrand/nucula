// In-memory NVS backing for the host test build (see shims/nvs.h).
#include "shims/nvs.h"

#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace {

struct Store {
    // namespace -> (key -> value bytes)
    std::map<std::string, std::map<std::string, std::string>> ns;
};

Store &store()
{
    static Store s;
    return s;
}

std::map<std::string, std::string> *ns_of(nvs_handle_t h)
{
    if (!h) return nullptr;
    return static_cast<std::map<std::string, std::string> *>(h);
}

} // namespace

extern "C" {

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    if (!ns || !out) return ESP_FAIL;
    *out = &store().ns[ns];
    return ESP_OK;
}

void nvs_close(nvs_handle_t) {}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    (*ns)[key] = val;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    auto it = ns->find(key);
    if (it == ns->end()) return ESP_ERR_NVS_NOT_FOUND;
    if (!len) return ESP_FAIL;
    if (!out) {
        *len = it->second.size() + 1;
        return ESP_OK;
    }
    if (*len < it->second.size() + 1) return ESP_FAIL;
    memcpy(out, it->second.c_str(), it->second.size() + 1);
    *len = it->second.size() + 1;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    (*ns)[key].assign(static_cast<const char *>(data), len);
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    auto it = ns->find(key);
    if (it == ns->end()) return ESP_ERR_NVS_NOT_FOUND;
    if (!len) return ESP_FAIL;
    if (!out) {
        *len = it->second.size();
        return ESP_OK;
    }
    if (*len < it->second.size()) return ESP_FAIL;
    memcpy(out, it->second.data(), it->second.size());
    *len = it->second.size();
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    (*ns)[key].assign(reinterpret_cast<const char *>(&v), sizeof(v));
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    auto it = ns->find(key);
    if (it == ns->end() || it->second.size() != sizeof(uint32_t))
        return ESP_ERR_NVS_NOT_FOUND;
    if (v) memcpy(v, it->second.data(), sizeof(uint32_t));
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    (*ns)[key].assign(reinterpret_cast<const char *>(&v), sizeof(v));
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    auto it = ns->find(key);
    if (it == ns->end() || it->second.size() != sizeof(uint8_t))
        return ESP_ERR_NVS_NOT_FOUND;
    if (v) memcpy(v, it->second.data(), sizeof(uint8_t));
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    return ns->erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    auto *ns = ns_of(h);
    if (!ns) return ESP_FAIL;
    ns->clear();
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t)
{
    return ESP_OK;
}

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    store().ns.clear();
    return ESP_OK;
}

} // extern "C"
