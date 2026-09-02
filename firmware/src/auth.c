#include "auth.h"
#include "config_manager.h"
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "auth";

bool auth_is_enabled(void) {
    gateway_config_t cfg;
    if (config_manager_load(&cfg)!=ESP_OK) return false;
    return cfg.local_token[0] != '\0';
}

bool auth_check(httpd_req_t *req) {
    gateway_config_t cfg;
    config_manager_load(&cfg);
    if (cfg.local_token[0]=='\0') return true; // open
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
    const char *prefix = "Bearer ";
    if (strncmp(hdr, prefix, strlen(prefix))!=0) return false;
    const char *tok = hdr + strlen(prefix);
    // constant-time compare
    size_t a_len = strlen(tok), b_len = strlen(cfg.local_token);
    if (a_len != b_len) return false;
    volatile int diff=0;
    for (size_t i=0;i<a_len;i++) diff |= tok[i] ^ cfg.local_token[i];
    bool ok = (diff==0);
    if (!ok) ESP_LOGW(TAG, "auth rejected");
    return ok;
}
