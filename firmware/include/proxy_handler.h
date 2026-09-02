#pragma once
#include "esp_http_server.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t proxy_handler_handle(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
