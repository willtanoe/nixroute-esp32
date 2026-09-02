#include "provider.h"
#include "config_manager.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

// Minimal registry — deepseek first, openai-compatible generic second
static const provider_t s_providers[] = {
    {
        .name = "deepseek",
        .base_url = "https://api.deepseek.com",
        .prefix = "deepseek-",
        .models = {"deepseek-chat","deepseek-reasoner"},
        .model_count = 2,
        .openai_compatible = true,
    },
    {
        .name = "openrouter",
        .base_url = "https://openrouter.ai",
        .prefix = "openrouter-",
        .models = {"openrouter-auto"},
        .model_count = 1,
        .openai_compatible = true,
    },
};

const provider_t* provider_find_for_model(const char *model) {
    if (!model) return &s_providers[0];
    for (size_t i=0;i<sizeof(s_providers)/sizeof(s_providers[0]);i++) {
        const provider_t *p = &s_providers[i];
        if (p->prefix && strncmp(model, p->prefix, strlen(p->prefix))==0) return p;
        for (size_t m=0;m<p->model_count;m++) if (strcmp(model, p->models[m])==0) return p;
    }
    return &s_providers[0]; // default
}
const provider_t* provider_get_by_name(const char *name) {
    for (size_t i=0;i<sizeof(s_providers)/sizeof(s_providers[0]);i++) if (strcmp(s_providers[i].name,name)==0) return &s_providers[i];
    return NULL;
}
int provider_list(const provider_t **out, size_t max) {
    size_t n = sizeof(s_providers)/sizeof(s_providers[0]);
    if (n>max) n=max;
    for (size_t i=0;i<n;i++) out[i]=&s_providers[i];
    return (int)n;
}

esp_err_t provider_get_auth_header(const provider_t *p, char *out, size_t out_len) {
    if (!p || !out) return ESP_ERR_INVALID_ARG;
    gateway_config_t cfg;
    config_manager_load(&cfg);
    const char *key = "";
    if (strcmp(p->name,"deepseek")==0) key = cfg.deepseek_key;
    else if (strcmp(p->name,"openrouter")==0) key = cfg.openrouter_key;
    if (key[0]=='\0') return ESP_ERR_NOT_FOUND;
    int n = snprintf(out, out_len, "Bearer %s", key);
    if (n <0 || (size_t)n >= out_len) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t provider_build_url(const provider_t *p, char *out, size_t out_len) {
    if (!p || !out) return ESP_ERR_INVALID_ARG;
    // DeepSeek: https://api.deepseek.com/v1/chat/completions
    // OpenRouter: https://openrouter.ai/api/v1/chat/completions
    if (strcmp(p->name,"openrouter")==0) {
        snprintf(out, out_len, "%s/api/v1/chat/completions", p->base_url);
    } else {
        snprintf(out, out_len, "%s/v1/chat/completions", p->base_url);
    }
    return ESP_OK;
}

bool provider_is_retryable(const provider_t *p, int http_status, esp_err_t err) {
    (void)p;
    if (err != ESP_OK) return true; // transport failure
    if (http_status==429 || http_status==500 || http_status==502 || http_status==503 || http_status==504) return true;
    return false;
}
