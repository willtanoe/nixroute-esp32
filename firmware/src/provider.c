#include "provider.h"
#include <string.h>

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
