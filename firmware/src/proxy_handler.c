#include "proxy_handler.h"
#include "config_manager.h"
#include "provider.h"
#include "routing.h"
#include "json_util.h"
#include "stats.h"
#include "auth.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "proxy";
static SemaphoreHandle_t s_mutex = NULL;

#define RESP_MAX 16384
typedef struct {
    char buf[RESP_MAX];
    size_t len;
    int status;
} proxy_resp_t;

typedef struct {
    httpd_req_t *req;
    bool headers_sent;
    bool failed;
    int status;
    size_t bytes;
} stream_ctx_t;

static esp_err_t client_event_handler(esp_http_client_event_t *evt) {
    proxy_resp_t *resp = (proxy_resp_t*)evt->user_data;
    if (!resp) return ESP_OK;
    if (evt->event_id==HTTP_EVENT_ON_DATA && evt->data_len) {
        size_t copy = evt->data_len;
        if (resp->len + copy >= RESP_MAX) {
            copy = RESP_MAX - resp->len - 1;
            if (copy==0) { ESP_LOGW(TAG, "truncated %d", RESP_MAX); return ESP_OK; }
        }
        memcpy(resp->buf + resp->len, evt->data, copy);
        resp->len += copy;
        resp->buf[resp->len]='\0';
    }
    return ESP_OK;
}

static esp_err_t stream_event_handler(esp_http_client_event_t *evt) {
    stream_ctx_t *ctx = (stream_ctx_t*)evt->user_data;
    if (!ctx) return ESP_OK;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            if (evt->data_len==0) break;
            // lazy header — first chunk sends headers
            // If not yet sent, we have already set headers via perform preamble
            // Just forward chunk
            esp_err_t err = httpd_resp_send_chunk(ctx->req, (const char*)evt->data, evt->data_len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "stream chunk send failed: %s", esp_err_to_name(err));
                ctx->failed = true;
                return ESP_FAIL;
            }
            ctx->bytes += evt->data_len;
            // watchdog feed every ~2KB
            if (ctx->bytes % 2048 == 0) vTaskDelay(pdMS_TO_TICKS(1));
            break;
        }
        case HTTP_EVENT_ON_FINISH:
        case HTTP_EVENT_ERROR:
        case HTTP_EVENT_DISCONNECTED:
            break;
        default: break;
    }
    return ESP_OK;
}

static void ensure_mutex(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

esp_err_t proxy_handler_handle(httpd_req_t *req) {
    ensure_mutex();
    if (auth_is_enabled() && !auth_check(req)) {
        const char *j="{\"error\":{\"message\":\"unauthorized\",\"type\":\"auth_error\",\"code\":401}}";
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        const char *j="{\"error\":{\"message\":\"too many concurrent requests — gateway serializes upstream\",\"type\":\"rate_limit\",\"code\":429}}";
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Retry-After", "1");
        return httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
    }
    int64_t start_us = esp_timer_get_time();
    gateway_config_t cfg;
    config_manager_load(&cfg);
    int max_body = cfg.max_body_bytes>0?cfg.max_body_bytes:8192;
    if (req->content_len > (size_t)max_body) {
        char j[128];
        snprintf(j,sizeof(j),"{\"error\":{\"message\":\"payload too large (%d > %d)\",\"type\":\"invalid_request\",\"code\":413}}", req->content_len, max_body);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
        xSemaphoreGive(s_mutex);
        gw_stats_on_request(false,0,false);
        return ESP_OK;
    }
    if (req->content_len==0) {
        const char *j="{\"error\":{\"message\":\"empty body\",\"type\":\"invalid_request\",\"code\":400}}";
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
        xSemaphoreGive(s_mutex);
        gw_stats_on_request(false,0,false);
        return ESP_OK;
    }
    size_t body_len = req->content_len;
    char *body = malloc(body_len+1);
    if (!body) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_send(req, "{\"error\":{\"message\":\"out of memory\",\"code\":500}}", HTTPD_RESP_USE_STRLEN);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    size_t received=0;
    while (received < body_len) {
        int ret = httpd_req_recv(req, body+received, body_len-received);
        if (ret<=0) {
            free(body);
            const char *j="{\"error\":{\"message\":\"client body read timeout\",\"type\":\"timeout\",\"code\":408}}";
            httpd_resp_set_status(req, "408 Request Timeout");
            httpd_resp_set_type(req, HTTPD_TYPE_JSON);
            httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
            xSemaphoreGive(s_mutex);
            gw_stats_on_request(false,0,false);
            return ESP_OK;
        }
        received+=ret;
    }
    body[body_len]='\0';
    ESP_LOGI(TAG, "POST len=%d heap=%d", (int)body_len, (int)esp_get_free_heap_size());
    char model[64]=""; bool is_stream=false;
    json_extract_model_stream(body, body_len, model, sizeof(model), &is_stream);
    ESP_LOGI(TAG, "model='%s' stream=%d", model[0]?model:"(none)", is_stream);
    routing_result_t route;
    routing_lookup(model[0]?model:"", &route);
    ESP_LOGI(TAG, "routing -> %d providers", (int)route.count);

    // streaming vs buffered
    bool want_stream = is_stream;
    proxy_resp_t resp={0};
    stream_ctx_t sctx={0};
    int last_status=0;
    esp_err_t last_err=ESP_OK;
    bool used_fallback=false;
    const provider_t *success_provider=NULL;
    bool streamed_success=false;

    for (size_t pi=0; pi<route.count; pi++) {
        const provider_t *p = provider_get_by_name(route.providers[pi]);
        if (!p) continue;
        char auth_hdr[128];
        if (provider_get_auth_header(p, auth_hdr, sizeof(auth_hdr))!=ESP_OK) {
            ESP_LOGW(TAG, "provider %s key missing", p->name);
            last_status=500; last_err=ESP_ERR_NOT_FOUND;
            if (pi+1<route.count) {used_fallback=true; continue; } break;
        }
        char url[160];
        provider_build_url(p, url, sizeof(url));
        ESP_LOGI(TAG, "upstream %s %s heap=%d", p->name, url, (int)esp_get_free_heap_size());

        if (want_stream) {
            // streaming path — headers prepared before perform
            // Must set headers before first chunk; httpd will send them on first send_chunk
            httpd_resp_set_status(req, HTTPD_200);
            httpd_resp_set_type(req, "text/event-stream");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            httpd_resp_set_hdr(req, "Connection", "keep-alive");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            // Note: we do NOT call httpd_resp_send; chunks will trigger headers
            sctx.req = req; sctx.failed=false; sctx.bytes=0; sctx.headers_sent=false;
            esp_http_client_config_t c={0};
            c.url=url; c.method=HTTP_METHOD_POST;
            c.timeout_ms = cfg.upstream_timeout_ms>0?cfg.upstream_timeout_ms:30000;
            c.buffer_size=1024; c.buffer_size_tx=1024;
            c.event_handler=stream_event_handler; c.user_data=&sctx;
            c.crt_bundle_attach=esp_crt_bundle_attach; c.keep_alive_enable=false;
            esp_http_client_handle_t client=esp_http_client_init(&c);
            if (!client) { last_err=ESP_ERR_NO_MEM; last_status=500; if(pi+1<route.count){used_fallback=true; continue;} break; }
            esp_http_client_set_header(client,"Content-Type","application/json");
            esp_http_client_set_header(client,"Authorization",auth_hdr);
            esp_http_client_set_header(client,"Accept","text/event-stream");
            if (strcmp(p->name,"openrouter")==0){esp_http_client_set_header(client,"HTTP-Referer","http://esp32.local"); esp_http_client_set_header(client,"X-Title","ESP32 Router");}
            esp_http_client_set_post_field(client, body, body_len);
            esp_err_t perr=esp_http_client_perform(client);
            int status=esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "stream upstream %s status=%d perr=%s bytes=%d", p->name, status, esp_err_to_name(perr), (int)sctx.bytes);
            esp_http_client_cleanup(client);
            bool retryable=false;
            if (sctx.failed || perr!=ESP_OK) { retryable=provider_is_retryable(p,0,perr!=ESP_OK?perr:ESP_FAIL); last_err=perr!=ESP_OK?perr:ESP_FAIL; last_status=0; }
            else { retryable=provider_is_retryable(p,status,ESP_OK); last_err=ESP_OK; last_status=status; }
            // Critical: if we already sent chunks (bytes>0), cannot fallback — connection already chunked
            if (sctx.bytes>0 && last_status!=200) {
                // already partially streamed error; just abort
                httpd_resp_send_chunk(req, NULL, 0);
                success_provider=p; streamed_success=true; // treat as attempt done
                break;
            }
            if (sctx.bytes>0 && sctx.failed) {
                // send terminate anyway
                httpd_resp_send_chunk(req, NULL, 0);
                break;
            }
            if (!sctx.failed && perr==ESP_OK && status>=200 && status<300) {
                // success streaming — terminate chunked
                httpd_resp_send_chunk(req, NULL, 0);
                success_provider=p; streamed_success=true;
                break;
            }
            // For streaming, if fallback possible and we haven't sent bytes, try next provider
            // Need to guarantee headers not yet committed? But we already set headers; second provider would try to set again — need to allow?
            // Simplify: once streaming attempted and failed before any byte, we can still try next provider by resetting? But httpd already has headers staged.
            // For WROOM-32 simplicity: if streaming attempt fails before first chunk, do fallback; else fail.
            if (sctx.bytes==0 && retryable && pi+1<route.count) {
                used_fallback=true;
                vTaskDelay(pdMS_TO_TICKS(100*(1<<pi)));
                continue;
            } else {
                // non-retryable or bytes already sent
                if (sctx.bytes==0) {
                    // haven't sent anything, convert to error JSON via normal path
                    // fall through to non-stream error handling by not marking streamed_success
                    streamed_success=false;
                } else {
                    // already sent chunks, consider success? no
                    httpd_resp_send_chunk(req, NULL, 0);
                    success_provider=p;
                    streamed_success=true;
                }
                break;
            }
        } else {
            // non-stream buffered path (as before)
            resp.len=0; resp.buf[0]='\0';
            esp_http_client_config_t c={0};
            c.url=url; c.method=HTTP_METHOD_POST;
            c.timeout_ms=cfg.upstream_timeout_ms>0?cfg.upstream_timeout_ms:15000;
            c.buffer_size=1024; c.buffer_size_tx=1024;
            c.event_handler=client_event_handler; c.user_data=&resp;
            c.crt_bundle_attach=esp_crt_bundle_attach; c.keep_alive_enable=false;
            esp_http_client_handle_t client=esp_http_client_init(&c);
            if (!client){last_err=ESP_ERR_NO_MEM; last_status=500; if(pi+1<route.count){used_fallback=true; continue;} break;}
            esp_http_client_set_header(client,"Content-Type","application/json");
            esp_http_client_set_header(client,"Authorization",auth_hdr);
            esp_http_client_set_header(client,"Accept","application/json, text/event-stream");
            if (strcmp(p->name,"openrouter")==0){esp_http_client_set_header(client,"HTTP-Referer","http://esp32.local"); esp_http_client_set_header(client,"X-Title","ESP32 Router");}
            esp_http_client_set_post_field(client, body, body_len);
            esp_err_t perr=esp_http_client_perform(client);
            int status=esp_http_client_get_status_code(client);
            resp.status=status;
            ESP_LOGI(TAG, "buffered upstream %s status=%d perr=%s len=%d", p->name, status, esp_err_to_name(perr), (int)resp.len);
            bool retryable=false;
            if (perr!=ESP_OK){retryable=provider_is_retryable(p,0,perr); last_err=perr; last_status=0;}
            else {retryable=provider_is_retryable(p,status,ESP_OK); last_err=ESP_OK; last_status=status;}
            esp_http_client_cleanup(client);
            if (perr==ESP_OK && status>=200 && status<300){success_provider=p; break;}
            if (retryable && pi+1<route.count){used_fallback=true; vTaskDelay(pdMS_TO_TICKS(100*(1<<pi))); continue;} else break;
        }
    }
    free(body);
    int32_t latency_ms=(int32_t)((esp_timer_get_time()-start_us)/1000);
    if (success_provider && (streamed_success || resp.len>0 || want_stream)) {
        if (want_stream && streamed_success) {
            ESP_LOGI(TAG, "stream success via %s bytes=%d latency=%dms", success_provider->name,(int)sctx.bytes,(int)latency_ms);
            gw_stats_on_request(true, latency_ms, used_fallback);
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
        if (!want_stream) {
            const char *ctype=HTTPD_TYPE_JSON;
            if (resp.len>=5 && strncmp(resp.buf,"data:",5)==0) ctype="text/event-stream";
            httpd_resp_set_status(req, HTTPD_200);
            httpd_resp_set_type(req, ctype);
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            if (resp.len==0) httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
            else httpd_resp_send(req, resp.buf, resp.len);
            ESP_LOGI(TAG, "proxy success %s latency=%d len=%d", success_provider->name,(int)latency_ms,(int)resp.len);
            gw_stats_on_request(true, latency_ms, used_fallback);
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
        // want_stream but not streamed_success means buffered SSE fallback — send buffered
        httpd_resp_set_status(req, HTTPD_200);
        httpd_resp_set_type(req, "text/event-stream");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, resp.buf, resp.len);
        gw_stats_on_request(true, latency_ms, used_fallback);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    } else {
        // failure path (only if nothing was streamed)
        if (want_stream && sctx.bytes>0) {
            // already handled
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
        int out_status=last_status?last_status:502;
        const char *body_to_send=NULL; char err_json[512];
        if (resp.len>0 && resp.buf[0]=='{') body_to_send=resp.buf;
        else {
            const char *msg="upstream_error";
            if(last_err==ESP_ERR_NOT_FOUND) msg="provider API key not configured";
            else if(last_err!=ESP_OK) msg=esp_err_to_name(last_err);
            else if(resp.len>0) msg="upstream error";
            snprintf(err_json,sizeof(err_json),"{\"error\":{\"message\":\"%s\",\"type\":\"upstream\",\"code\":%d,\"provider_status\":%d}}", msg,out_status,last_status);
            body_to_send=err_json;
        }
        if(out_status==429) httpd_resp_set_status(req,"429 Too Many Requests");
        else if(out_status==500) httpd_resp_set_status(req,HTTPD_500);
        else if(out_status==502) httpd_resp_set_status(req,"502 Bad Gateway");
        else if(out_status==503) httpd_resp_set_status(req,"503 Service Unavailable");
        else if(out_status==504) httpd_resp_set_status(req,"504 Gateway Timeout");
        else if(out_status==400) httpd_resp_set_status(req,HTTPD_400);
        else if(out_status==401) httpd_resp_set_status(req,"401 Unauthorized");
        else if(out_status==404) httpd_resp_set_status(req,HTTPD_404);
        else httpd_resp_set_status(req,"502 Bad Gateway");
        httpd_resp_set_type(req,HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");
        httpd_resp_send(req, body_to_send, HTTPD_RESP_USE_STRLEN);
        ESP_LOGW(TAG, "proxy failed out=%d last=%d err=%s", out_status,last_status,esp_err_to_name(last_err));
        gw_stats_on_request(false, latency_ms, used_fallback);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
}
