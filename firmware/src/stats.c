#include "stats.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <string.h>

static int64_t s_boot_us;
static uint32_t s_total=0, s_success=0, s_failed=0, s_fallbacks=0;
static int32_t s_last_latency=0;

void gw_stats_init(void) { s_boot_us = esp_timer_get_time(); }
void gw_stats_on_request(bool success, int32_t latency_ms, bool fallback) {
    s_total++;
    if (success) s_success++; else s_failed++;
    if (fallback) s_fallbacks++;
    s_last_latency = latency_ms;
}
void gw_stats_get(gateway_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->uptime_s = (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000000);
    out->free_heap = (int)esp_get_free_heap_size();
    out->min_free_heap = (int)esp_get_minimum_free_heap_size();
    out->requests_total = s_total;
    out->requests_success = s_success;
    out->requests_failed = s_failed;
    out->fallbacks = s_fallbacks;
    out->last_latency_ms = s_last_latency;
    // wifi fields filled by caller via wifi_manager where needed
    extern bool wifi_manager_is_connected(void);
    extern uint32_t wifi_manager_get_disconnects(void);
    extern int8_t wifi_manager_get_rssi(void);
    extern esp_err_t wifi_manager_get_ip(char*,size_t);
    out->wifi_connected = wifi_manager_is_connected();
    out->wifi_disconnects = wifi_manager_get_disconnects();
    out->wifi_rssi = wifi_manager_get_rssi();
    wifi_manager_get_ip(out->ip_str, sizeof(out->ip_str));
}
