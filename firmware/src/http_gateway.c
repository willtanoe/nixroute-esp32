#include "http_gateway.h"
#include "esp_log.h"

static const char *TAG = "http";

esp_err_t http_gateway_start(int port) {
    ESP_LOGI(TAG, "Phase 1 stub: http_gateway not yet started (Phase 3) port=%d", port);
    return ESP_OK;
}
void http_gateway_stop(void) {
    ESP_LOGI(TAG, "http stop stub");
}
