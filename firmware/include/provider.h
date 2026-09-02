#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct provider_s {
    const char *name;
    const char *base_url;
    const char *prefix;           // model prefix e.g. "deepseek-"
    const char *models[4];
    size_t      model_count;
    bool        openai_compatible;
    // auth header built from NVS key at runtime
    const char* (*auth_header)(char *out, size_t out_len);
    bool        (*is_retryable)(int http_status, esp_err_t transport_err);
} provider_t;

const provider_t* provider_find_for_model(const char *model);
const provider_t* provider_get_by_name(const char *name);
int               provider_list(const provider_t **out, size_t max);

#ifdef __cplusplus
}
#endif
