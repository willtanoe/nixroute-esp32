#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";
static bool s_connected = false;
static uint32_t s_disconnects = 0;

esp_err_t wifi_manager_init(void) {
    ESP_LOGI(TAG, "Phase 1 stub: wifi_manager not yet implemented (Phase 2)");
    // Real init moved to Phase 2: event loop, netif, esp_wifi_start
    return ESP_OK;
}

bool wifi_manager_is_connected(void) { return s_connected; }
esp_err_t wifi_manager_get_ip(char *out, size_t len) {
    if (out && len) snprintf(out, len, "0.0.0.0");
    return ESP_OK;
}
int8_t wifi_manager_get_rssi(void) { return 0; }
uint32_t wifi_manager_get_disconnects(void) { return s_disconnects; }
