#include "routing.h"
#include "provider.h"
#include <string.h>

bool routing_lookup(const char *model, routing_result_t *out) {
    if (!out) return false;
    const provider_t *p = provider_find_for_model(model);
    out->providers[0] = p->name;
    out->count = 1;
    out->timeout_ms = 15000;
    // Add fallback: deepseek→openrouter (if distinct)
    if (strcmp(p->name,"deepseek")==0) {
        out->providers[1] = "openrouter";
        out->count = 2;
    }
    return true;
}
