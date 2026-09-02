#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Ordered list of provider names to try for a given model
typedef struct {
    const char *providers[3];
    size_t count;
    int timeout_ms;
} routing_result_t;

bool routing_lookup(const char *model, routing_result_t *out);

#ifdef __cplusplus
}
#endif
