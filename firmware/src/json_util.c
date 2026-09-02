#include "json_util.h"
#include <string.h>
#include <ctype.h>

// Minimal bounded scan: looks for "model":"..." and "stream":true/false
// Avoids pulling cJSON for now; validates only needed fields.
// Phase 5 will switch to cJSON for full validation if heap allows.

bool json_extract_model_stream(const char *json, size_t len,
                               char *model_out, size_t model_cap,
                               bool *stream_out) {
    if (!json || !model_out || !stream_out) return false;
    model_out[0]='\0';
    *stream_out=false;

    // find "model"
    const char *mkey = "\"model\"";
    const char *p = NULL;
    // naive scan within len
    for (size_t i=0; i+7 < len; i++) {
        if (strncmp(json+i, mkey, 7)==0) { p = json+i+7; break; }
    }
    if (p) {
        // skip whitespace, colon, whitespace, quote
        while (p < json+len && (isspace((unsigned char)*p) || *p==':')) p++;
        if (p < json+len && *p=='"') {
            p++;
            size_t o=0;
            while (p < json+len && *p!='"' && o+1 < model_cap) {
                model_out[o++] = *p++;
            }
            model_out[o]='\0';
        }
    }
    // find "stream"
    const char *skey="\"stream\"";
    const char *s=NULL;
    for (size_t i=0;i+8 < len;i++) if (strncmp(json+i,skey,8)==0){s=json+i+8;break;}
    if (s) {
        while (s<json+len && (isspace((unsigned char)*s)||*s==':')) s++;
        if (s+4<=json+len && strncmp(s,"true",4)==0) *stream_out=true;
        else *stream_out=false;
    }
    return true;
}
