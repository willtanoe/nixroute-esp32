#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_gateway_start(int port);
void      http_gateway_stop(void);

#ifdef __cplusplus
}
#endif
