#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_SSID_MAX        33
#define CFG_PASS_MAX        65
#define CFG_TOKEN_MAX       65
#define CFG_KEY_MAX         85
#define CFG_ROUTING_MAX     256

typedef struct {
    char wifi_ssid[CFG_SSID_MAX];
    char wifi_password[CFG_PASS_MAX];
    char local_token[CFG_TOKEN_MAX];
    char deepseek_key[CFG_KEY_MAX];
    char openrouter_key[CFG_KEY_MAX];
    char routing_json[CFG_ROUTING_MAX];
    int  http_port;
    int  upstream_timeout_ms;
    int  max_body_bytes;
} gateway_config_t;

esp_err_t config_manager_init(void);
esp_err_t config_manager_load(gateway_config_t *out);
esp_err_t config_manager_save(const gateway_config_t *cfg);
void      config_manager_log_sanitized(const gateway_config_t *cfg);

#ifdef __cplusplus
}
#endif
