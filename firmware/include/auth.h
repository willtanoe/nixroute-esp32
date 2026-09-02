#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

bool auth_check(httpd_req_t *req);
bool auth_is_enabled(void);

#ifdef __cplusplus
}
#endif
