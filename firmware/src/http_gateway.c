#include "http_gateway.h"
#include "wifi_manager.h"
#include "stats.h"
#include "config_manager.h"
#include "provider.h"
#include "auth.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;

static esp_err_t send_json(httpd_req_t *req, const char *json, int status_code) {
    if (status_code==200) httpd_resp_set_status(req, HTTPD_200);
    else if (status_code==401) httpd_resp_set_status(req, "401 Unauthorized");
    else if (status_code==404) httpd_resp_set_status(req, HTTPD_404);
    else if (status_code==429) httpd_resp_set_status(req, "429 Too Many Requests");
    else if (status_code==501) httpd_resp_set_status(req, "501 Not Implemented");
    else if (status_code==500) httpd_resp_set_status(req, HTTPD_500);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// --- /health ---
static esp_err_t health_get_handler(httpd_req_t *req) {
    char ip[16]; wifi_manager_get_ip(ip, sizeof(ip));
    bool wifi_ok = wifi_manager_is_connected();
    int free_heap = (int)esp_get_free_heap_size();
    int min_heap = (int)esp_get_minimum_free_heap_size();
    int64_t up_s = esp_timer_get_time() / 1000000;
    gateway_stats_t st; stats_get(&st);

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"uptime_s\":%lld,\"wifi_connected\":%s,\"ip\":\"%s\",\"rssi\":%d,"
        "\"free_heap\":%d,\"min_heap\":%d,\"requests_total\":%u,\"heap_largest\":%d}",
        wifi_ok ? "ok" : "wifi_disconnected",
        (long long)up_s,
        wifi_ok ? "true" : "false",
        ip,
        (int)wifi_manager_get_rssi(),
        free_heap, min_heap,
        (unsigned)st.requests_total,
        (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
    );
    (void)n;
    ESP_LOGI(TAG, "GET /health -> wifi=%d ip=%s heap=%d", wifi_ok, ip, free_heap);
    return send_json(req, buf, 200);
}

// --- /v1/models ---
static esp_err_t models_get_handler(httpd_req_t *req) {
    if (auth_is_enabled() && !auth_check(req)) {
        return send_json(req, "{\"error\":{\"message\":\"unauthorized\",\"type\":\"auth_error\"}}", 401);
    }
    const provider_t *list[8];
    int n = provider_list(list, 8);
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "{\"object\":\"list\",\"data\":[");
    for (int i=0;i<n;i++) {
        for (size_t m=0; m<list[i]->model_count && off < (int)sizeof(buf)-128; m++) {
            if (i!=0 || m!=0) off += snprintf(buf+off, sizeof(buf)-off, ",");
            off += snprintf(buf+off, sizeof(buf)-off,
                "{\"id\":\"%s\",\"object\":\"model\",\"owned_by\":\"%s\"}",
                list[i]->models[m], list[i]->name);
        }
    }
    snprintf(buf+off, sizeof(buf)-off, "]}");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return send_json(req, buf, 200);
}

// --- /admin/status ---
static esp_err_t admin_status_handler(httpd_req_t *req) {
    if (auth_is_enabled() && !auth_check(req)) {
        return send_json(req, "{\"error\":\"unauthorized\"}", 401);
    }
    char ip[16]; wifi_manager_get_ip(ip,sizeof(ip));
    gateway_stats_t st; stats_get(&st);
    char buf[600];
    snprintf(buf,sizeof(buf),
        "{\"uptime_s\":%u,\"free_heap\":%d,\"min_heap\":%d,\"wifi_connected\":%s,\"ip\":\"%s\",\"rssi\":%d,\"disconnects\":%u}",
        (unsigned)st.uptime_s, st.free_heap, st.min_free_heap,
        st.wifi_connected ? "true":"false",
        ip, (int)wifi_manager_get_rssi(), (unsigned)wifi_manager_get_disconnects());
    return send_json(req, buf, 200);
}

static esp_err_t admin_stats_handler(httpd_req_t *req) {
    if (auth_is_enabled() && !auth_check(req)) return send_json(req, "{\"error\":\"unauthorized\"}", 401);
    gateway_stats_t st; stats_get(&st);
    char buf[512];
    snprintf(buf,sizeof(buf),
        "{\"requests_total\":%u,\"requests_success\":%u,\"requests_failed\":%u,\"fallbacks\":%u,\"free_heap\":%d,\"min_heap\":%d,\"last_latency_ms\":%d}",
        (unsigned)st.requests_total, (unsigned)st.requests_success, (unsigned)st.requests_failed, (unsigned)st.fallbacks, st.free_heap, st.min_free_heap, (int)st.last_latency_ms);
    return send_json(req, buf, 200);
}

static esp_err_t admin_providers_handler(httpd_req_t *req) {
    if (auth_is_enabled() && !auth_check(req)) return send_json(req, "{\"error\":\"unauthorized\"}", 401);
    const provider_t *list[8]; int n=provider_list(list,8);
    char buf[800]; int off=snprintf(buf,sizeof(buf),"{\"providers\":[");
    for(int i=0;i<n;i++){
        if(i) off+=snprintf(buf+off,sizeof(buf)-off,",");
        off+=snprintf(buf+off,sizeof(buf)-off,"{\"name\":\"%s\",\"base_url\":\"%s\",\"openai_compatible\":%s}",
                      list[i]->name, list[i]->base_url, list[i]->openai_compatible?"true":"false");
    }
    snprintf(buf+off,sizeof(buf)-off,"]}");
    return send_json(req, buf, 200);
}

// --- POST /v1/chat/completions (Phase 3: 501 until proxy lands in Phase 4) ---
static esp_err_t chat_completions_handler(httpd_req_t *req) {
    if (auth_is_enabled() && !auth_check(req)) {
        return send_json(req, "{\"error\":{\"message\":\"unauthorized\",\"type\":\"auth_error\",\"code\":401}}", 401);
    }
    // For Phase 3, return 501 but valid OpenAI error shape so SDKs show "not ready"
    // Phase 4 will replace this with real proxy logic.
    // Still read and drain body to avoid client hanging
    char tmp[256];
    size_t to_read = req->content_len;
    while (to_read>0) {
        int ret = httpd_req_recv(req, tmp, to_read > sizeof(tmp) ? sizeof(tmp) : to_read);
        if (ret <=0) break;
        to_read -= ret;
    }
    ESP_LOGW(TAG, "POST /v1/chat/completions called — proxy not yet (Phase 4) content_len=%d", req->content_len);
    return send_json(req,
        "{\"error\":{\"message\":\"proxy not yet implemented — Phase 4 pending. /health and /v1/models work.\",\"type\":\"not_implemented\",\"code\":501}}",
        501);
}

// CORS preflight
static esp_err_t options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_status(req, HTTPD_204);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_status(req, HTTPD_404);
    const char *msg = "{\"error\":{\"message\":\"not found\",\"type\":\"not_found\",\"code\":404}}";
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

esp_err_t http_gateway_start(int port) {
    if (s_server) {
        ESP_LOGW(TAG, "http server already running");
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port >0 ? port : 80;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 12;
    config.stack_size = 6144;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;
    // keep control port default

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Health (no auth)
    httpd_uri_t h = { .uri="/health", .method=HTTP_GET, .handler=health_get_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &h);
    // Models
    httpd_uri_t m = { .uri="/v1/models", .method=HTTP_GET, .handler=models_get_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &m);
    // Admin
    httpd_uri_t as = { .uri="/admin/status", .method=HTTP_GET, .handler=admin_status_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &as);
    httpd_uri_t ap = { .uri="/admin/providers", .method=HTTP_GET, .handler=admin_providers_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &ap);
    httpd_uri_t ast = { .uri="/admin/stats", .method=HTTP_GET, .handler=admin_stats_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &ast);
    // Chat completions
    httpd_uri_t chat = { .uri="/v1/chat/completions", .method=HTTP_POST, .handler=chat_completions_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &chat);
    // CORS OPTIONS
    httpd_uri_t opt1 = { .uri="/v1/*", .method=HTTP_OPTIONS, .handler=options_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &opt1);
    httpd_uri_t opt2 = { .uri="/health", .method=HTTP_OPTIONS, .handler=options_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &opt2);
    httpd_uri_t opt3 = { .uri="/admin/*", .method=HTTP_OPTIONS, .handler=options_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &opt3);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);

    ESP_LOGI(TAG, "HTTP gateway started — routes: /health, /v1/models, /admin/*, /v1/chat/completions (501 stub)");
    ESP_LOGI(TAG, "heap after httpd: free=%d min=%d", (int)esp_get_free_heap_size(), (int)esp_get_minimum_free_heap_size());
    return ESP_OK;
}

void http_gateway_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP gateway stopped");
    }
}
