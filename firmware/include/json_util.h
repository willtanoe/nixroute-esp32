#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extract "model" and "stream" from a bounded JSON buffer without full duplication
// Returns true if extraction succeeded (model may be empty if absent)
bool json_extract_model_stream(const char *json, size_t len,
                               char *model_out, size_t model_cap,
                               bool *stream_out);

#ifdef __cplusplus
}
#endif
