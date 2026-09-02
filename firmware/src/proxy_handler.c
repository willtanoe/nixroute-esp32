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
#include <string.h>
#include <stdlib.h>

static const char *TAG = "proxy";
static SemaphoreHandle_t s_mutex = NULL;

// Response accumulation for non-streaming
#define RESP_MAX 16384
typedef struct {
    char buf[RESP_MAX];
    size_t len;
    int status;
    bool is_stream; // provider returned text/event-stream
} proxy_resp_t;

static esp_err_t client_event_handler(esp_http_client_event_t *evt) {
    proxy_resp_t *resp = (proxy_resp_t*)evt->user_data;
    if (!resp) return ESP_OK;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER: {
            // detect content-type for streaming detection if needed
            break;
        }
        case HTTP_EVENT_ON_DATA: {
            if (evt->data_len==0) break;
            size_t copy = evt->data_len;
            if (resp->len + copy >= RESP_MAX) {
                // truncate — upstream too large, log and clamp
                copy = RESP_MAX - resp->len - 1;
                if (copy==0) {
                    ESP_LOGW(TAG, "upstream response truncated at %d bytes", RESP_MAX);
                    break;
                }
            }
            memcpy(resp->buf + resp->len, evt->data, copy);
            resp->len += copy;
            resp->buf[resp->len]='\0';
            break;
        }
        default: break;
    }
    return ESP_OK;
}

static void ensure_mutex(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

esp_err_t proxy_handler_handle(httpd_req_t *req) {
    ensure_mutex();
    // auth
    if (auth_is_enabled() && !auth_check(req)) {
        const char *j="{\"error\":{\"message\":\"unauthorized\",\"type\":\"auth_error\",\"code\":401}}";
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
    }

    // serialize upstream (one TLS at a time)
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
    int max_body = cfg.max_body_bytes >0 ? cfg.max_body_bytes : 8192;
    if (req->content_len > (size_t)max_body) {
        char j[128];
        snprintf(j,sizeof(j),"{\"error\":{\"message\":\"payload too large (%d > %d)\",\"type\":\"invalid_request\",\"code\":413}}", req->content_len, max_body);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
        xSemaphoreGive(s_mutex);
        gw_stats_on_request(false, 0, false);
        return ESP_OK;
    }
    if (req->content_len==0) {
        const char *j="{\"error\":{\"message\":\"empty body\",\"type\":\"invalid_request\",\"code\":400}}";
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
        xSemaphoreGive(s_mutex);
        gw_stats_on_request(false, 0, false);
        return ESP_OK;
    }

    size_t body_len = req->content_len;
    char *body = (char*)malloc(body_len+1);
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
        if (ret <=0) {
            if (ret==HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "httpd_req_recv timeout");
            }
            free(body);
            const char *j="{\"error\":{\"message\":\"client body read timeout\",\"type\":\"timeout\",\"code\":408}}";
            httpd_resp_set_status(req, "408 Request Timeout");
            httpd_resp_set_type(req, HTTPD_TYPE_JSON);
            httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
            xSemaphoreGive(s_mutex);
            gw_stats_on_request(false, 0, false);
            return ESP_OK;
        }
        received += ret;
    }
    body[body_len]='\0';
    ESP_LOGI(TAG, "POST /v1/chat/completions len=%d heap=%d", (int)body_len, (int)esp_get_free_heap_size());

    // extract model + stream
    char model[64]=""; bool is_stream=false;
    json_extract_model_stream(body, body_len, model, sizeof(model), &is_stream);
    ESP_LOGI(TAG, "model='%s' stream=%d", model[0]?model:"(none)", is_stream);

    // Stream not yet supported in this phase; handle after Phase 9 stub
    if (is_stream) {
        // For now, we forward but non-streaming path will buffer; warn
        // Future streaming implementation will do chunked forwarding.
        // Keep flag but proceed as non-streaming (provider will return SSE;
        // we will buffer and send as single SSE payload — not ideal but functional for small streams)
        ESP_LOGW(TAG, "stream=true requested — buffering as non-stream (streaming Phase 9 pending)");
    }

    // routing
    routing_result_t route;
    routing_lookup(model[0]?model:"", &route);
    ESP_LOGI(TAG, "routing: model='%s' -> %d provider(s)", model, (int)route.count);

    // Try providers in order
    proxy_resp_t resp = {0};
    int last_status=0;
    esp_err_t last_err=ESP_OK;
    bool used_fallback=false;
    const provider_t *success_provider=NULL;

    for (size_t pi=0; pi< route.count; pi++) {
        const provider_t *p = provider_get_by_name(route.providers[pi]);
        if (!p) { ESP_LOGW(TAG, "provider '%s' unknown", route.providers[pi]); continue; }

        char auth_hdr[128];
        if (provider_get_auth_header(p, auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
            ESP_LOGW(TAG, "provider %s key not configured — skipping", p->name);
            last_status=500;
            last_err=ESP_ERR_NOT_FOUND;
            if (pi+1 < route.count) { used_fallback=true; continue; }
            break;
        }
        char url[160];
        provider_build_url(p, url, sizeof(url));
        ESP_LOGI(TAG, "upstream %s -> %s", p->name, url);
        resp.len=0; resp.status=0; resp.buf[0]='\0';

        esp_http_client_config_t c = {0};
        c.url = url;
        c.method = HTTP_METHOD_POST;
        c.timeout_ms = cfg.upstream_timeout_ms >0 ? cfg.upstream_timeout_ms : 15000;
        c.buffer_size = 1024;
        c.buffer_size_tx = 1024;
        c.event_handler = client_event_handler;
        c.user_data = &resp;
        c.crt_bundle_attach = esp_crt_bundle_attach;
        c.keep_alive_enable = false;

        esp_http_client_handle_t client = esp_http_client_init(&c);
        if (!client) {
            last_err=ESP_ERR_NO_MEM;
            last_status=500;
            if (pi+1 < route.count) { used_fallback=true; continue; }
            break;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Authorization", auth_hdr);
        esp_http_client_set_header(client, "Accept", "application/json, text/event-stream");
        // OpenRouter needs HTTP-Referer / X-Title optional
        if (strcmp(p->name,"openrouter")==0) {
            esp_http_client_set_header(client, "HTTP-Referer", "http://esp32.local");
            esp_http_client_set_header(client, "X-Title", "ESP32 Router");
        }
        esp_http_client_set_post_field(client, body, body_len);

        // If body contained stream:true and provider is OpenAI-compatible, upstreams will reply SSE;
        // our event handler will capture whatever bytes (buffered) — Phase 9 will stream chunked.
        esp_err_t perr = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        // also fetch via header? but perform already did events
        // If perform returned ESP_OK but status 0 means no response?
        resp.status = status;
        ESP_LOGI(TAG, "upstream %s status=%d perr=%s len=%d heap=%d",
                 p->name, status, esp_err_to_name(perr), (int)resp.len, (int)esp_get_free_heap_size());

        bool should_retry = false;
        if (perr != ESP_OK) {
            should_retry = provider_is_retryable(p, 0, perr);
            last_err = perr;
            last_status = 0;
        } else {
            should_retry = provider_is_retryable(p, status, ESP_OK);
            last_err = ESP_OK;
            last_status = status;
        }

        esp_http_client_cleanup(client);

        if (perr==ESP_OK && status>=200 && status<300) {
            success_provider = p;
            break;
        }
        if (should_retry && pi+1 < route.count) {
            used_fallback = true;
            // small backoff
            vTaskDelay(pdMS_TO_TICKS(100 * (1<<pi)));
            ESP_LOGI(TAG, "retry -> fallback to next provider");
            continue;
        } else {
            // non-retryable or last
            break;
        }
    }

    free(body);

    int32_t latency_ms = (int32_t)((esp_timer_get_time() - start_us)/1000);
    if (success_provider) {
        // success — forward provider body verbatim with provider's Content-Type detection
        // If upstream was SSE (starts with data:), keep text/event-stream, else json
        const char *ctype = HTTPD_TYPE_JSON;
        if (resp.len >=5 && strncmp(resp.buf, "data:", 5)==0) ctype = "text/event-stream";
        httpd_resp_set_status(req, HTTPD_200);
        httpd_resp_set_type(req, ctype);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        if (resp.len==0) {
            // empty? shouldn't happen
            httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
        } else {
            httpd_resp_send(req, resp.buf, resp.len);
        }
        ESP_LOGI(TAG, "proxy success via %s latency=%dms len=%d", success_provider->name, (int)latency_ms, (int)resp.len);
        gw_stats_on_request(true, latency_ms, used_fallback);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    } else {
        // failure — map upstream error to gateway error (preserve status shape if upstream gave JSON)
        int out_status = last_status ? last_status : 502;
        const char *body_to_send = NULL;
        char err_json[512];
        if (resp.len>0 && resp.buf[0]=='{') {
            // forward provider error JSON verbatim (contains type/message)
            body_to_send = resp.buf;
        } else {
            // synthesize OpenAI-shaped error
            int code = out_status;
            const char *msg = "upstream_error";
            if (last_err==ESP_ERR_NOT_FOUND) msg="provider API key not configured";
            else if (last_err!=ESP_OK) msg=esp_err_to_name(last_err);
            else if (resp.len>0) msg="upstream returned error";
            snprintf(err_json,sizeof(err_json),
                "{\"error\":{\"message\":\"%s\",\"type\":\"upstream\",\"code\":%d,\"provider_status\":%d}}",
                msg, code, last_status);
            body_to_send = err_json;
        }
        if (out_status==429) httpd_resp_set_status(req, "429 Too Many Requests");
        else if (out_status==500) httpd_resp_set_status(req, HTTPD_500);
        else if (out_status==502) httpd_resp_set_status(req, "502 Bad Gateway");
        else if (out_status==503) httpd_resp_set_status(req, "503 Service Unavailable");
        else if (out_status==504) httpd_resp_set_status(req, "504 Gateway Timeout");
        else if (out_status==400) httpd_resp_set_status(req, HTTPD_400);
        else if (out_status==401) httpd_resp_set_status(req, "401 Unauthorized");
        else if (out_status==404) httpd_resp_set_status(req, HTTPD_404);
        else httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, body_to_send, HTTPD_RESP_USE_STRLEN);
        ESP_LOGW(TAG, "proxy failed out_status=%d last=%d err=%s stream_fallback=%d", out_status, last_status, esp_err_to_name(last_err), used_fallback);
        gw_stats_on_request(false, latency_ms, used_fallback);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
}
