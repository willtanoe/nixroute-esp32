#include "stats.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <string.h>

static int64_t s_boot_us;

void stats_init(void) { s_boot_us = esp_timer_get_time(); }
void stats_on_request(bool success, int32_t latency_ms, bool fallback) { (void)success; (void)latency_ms; (void)fallback; }
void stats_get(gateway_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->uptime_s = (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000000);
    out->free_heap = (int)esp_get_free_heap_size();
    out->min_free_heap = (int)esp_get_minimum_free_heap_size();
}
