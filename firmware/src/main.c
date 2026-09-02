/**
 * ESP32-WROOM-32 AI API Router — Phase 1 skeleton
 * Boots, inits NVS, logs heap, then idles. Wi-Fi + HTTP come in Phase 2/3.
 * This file intentionally minimal so "pio run" succeeds and heap is measurable.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "config_manager.h"
#include "stats.h"
#include "wifi_manager.h"
#include "http_gateway.h"

static const char *TAG = "gateway";

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32 AI API Router — Phase 1 skeleton ===");
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "heap free: %d  min: %d  largest: %d",
             (int)esp_get_free_heap_size(),
             (int)esp_get_minimum_free_heap_size(),
             (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized");

    ESP_LOGI(TAG, "heap after NVS: free=%d min=%d",
             (int)esp_get_free_heap_size(),
             (int)esp_get_minimum_free_heap_size());

    gateway_config_t cfg;
    config_manager_load(&cfg);
    config_manager_log_sanitized(&cfg);
    gw_stats_init();
    wifi_manager_init();
    http_gateway_start(cfg.http_port);

    // Phase 1 gate: stable idle loop — proves boot + FreerTOS + heap without Wi-Fi.
    // Wi-Fi manager will replace this delay in Phase 2.
    ESP_LOGI(TAG, "Phase 1 skeleton running — will idle, heap heartbeat every 10s");
    int64_t start_us = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        int64_t up_s = (esp_timer_get_time() - start_us) / 1000000;
        ESP_LOGI(TAG, "heartbeat uptime=%llds free=%d min=%d largest=%d",
                 up_s,
                 (int)esp_get_free_heap_size(),
                 (int)esp_get_minimum_free_heap_size(),
                 (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
}
