#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(void);
bool      wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char *out, size_t len);
int8_t    wifi_manager_get_rssi(void);
uint32_t  wifi_manager_get_disconnects(void);

#ifdef __cplusplus
}
#endif
