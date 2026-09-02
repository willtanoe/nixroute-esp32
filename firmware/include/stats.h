#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t uptime_s;
    uint32_t requests_total;
    uint32_t requests_success;
    uint32_t requests_failed;
    uint32_t fallbacks;
    int      free_heap;
    int      min_free_heap;
    int32_t  last_latency_ms;
    bool     wifi_connected;
    int8_t   wifi_rssi;
    char     ip_str[16];
    uint32_t wifi_disconnects;
} gateway_stats_t;

void stats_init(void);
void stats_on_request(bool success, int32_t latency_ms, bool fallback);
void stats_get(gateway_stats_t *out);

#ifdef __cplusplus
}
#endif
