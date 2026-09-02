#include "config_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "config";
static const char *NVS_NS = "gateway";

esp_err_t config_manager_init(void) {
    // NSS already init'd in app_main; keep idempotent
    return ESP_OK;
}

esp_err_t config_manager_load(gateway_config_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    // defaults (overridden by NVS)
    out->http_port = 80;
    out->upstream_timeout_ms = 15000;
    out->max_body_bytes = 8192;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK; // use defaults
    size_t len;
    len = sizeof(out->wifi_ssid);
    nvs_get_str(h, "wifi_ssid", out->wifi_ssid, &len);
    len = sizeof(out->wifi_password);
    nvs_get_str(h, "wifi_pass", out->wifi_password, &len);
    len = sizeof(out->deepseek_key);
    nvs_get_str(h, "ds_key", out->deepseek_key, &len);
    len = sizeof(out->openrouter_key);
    nvs_get_str(h, "or_key", out->openrouter_key, &len);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_manager_save(const gateway_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    if (cfg->wifi_ssid[0]) nvs_set_str(h, "wifi_ssid", cfg->wifi_ssid);
    if (cfg->wifi_password[0]) nvs_set_str(h, "wifi_pass", cfg->wifi_password);
    if (cfg->deepseek_key[0]) nvs_set_str(h, "ds_key", cfg->deepseek_key);
    if (cfg->openrouter_key[0]) nvs_set_str(h, "or_key", cfg->openrouter_key);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config saved: %s", esp_err_to_name(err));
    return err;
}

void config_manager_log_sanitized(const gateway_config_t *cfg) {
    ESP_LOGI(TAG, "config: ssid=%s port=%d timeout=%d max_body=%d ds_key=%s or_key=%s",
             cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(empty)",
             cfg->http_port, cfg->upstream_timeout_ms, cfg->max_body_bytes,
             cfg->deepseek_key[0] ? "***" : "(empty)",
             cfg->openrouter_key[0] ? "***" : "(empty)");
}
