#include "wifi_manager.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi";

static bool s_connected = false;
static bool s_initialized = false;
static uint32_t s_disconnects = 0;
static uint32_t s_reconnect_attempt = 0;
static char s_ip_str[16] = "0.0.0.0";
static esp_netif_t *s_netif = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;

static void reconnect_timer_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "reconnect attempt %u", (unsigned)(s_reconnect_attempt+1));
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        // reschedule with backoff
        if (s_reconnect_timer) {
            uint32_t delay_ms = (1000u << s_reconnect_attempt);
            if (delay_ms > 30000) delay_ms = 30000;
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
            if (s_reconnect_attempt < 5) s_reconnect_attempt++;
        }
    }
}

static void schedule_reconnect(void) {
    if (!s_reconnect_timer) return;
    uint32_t delay_ms = (1000u << s_reconnect_attempt);
    if (delay_ms > 30000) delay_ms = 30000;
    if (s_reconnect_attempt < 5) s_reconnect_attempt++;
    ESP_LOGI(TAG, "scheduling reconnect in %ums (attempt %u)", delay_ms, (unsigned)s_reconnect_attempt);
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
}

static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START -> connect");
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t*)data;
            s_connected = false;
            s_disconnects++;
            s_ip_str[0]='\0'; strncpy(s_ip_str, "0.0.0.0", sizeof(s_ip_str));
            ESP_LOGW(TAG, "disconnected reason=%d ssid=%.*s rssi=%d (total %u)",
                     ev->reason, ev->ssid_len, (char*)ev->ssid, ev->rssi, (unsigned)s_disconnects);
            schedule_reconnect();
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED");
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *ev = (ip_event_got_ip_t*)data;
            s_connected = true;
            s_reconnect_attempt = 0;
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
            ESP_LOGI(TAG, "GOT IP " IPSTR " (gw " IPSTR ")",
                     IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
            ESP_LOGI(TAG, "heap after CONNECT: free=%d min=%d", (int)esp_get_free_heap_size(), (int)esp_get_minimum_free_heap_size());
        } else if (id == IP_EVENT_STA_LOST_IP) {
            s_connected = false;
            ESP_LOGW(TAG, "LOST IP");
        }
    }
}

esp_err_t wifi_manager_init(void) {
    if (s_initialized) return ESP_OK;

    gateway_config_t cfg;
    config_manager_load(&cfg);
    if (cfg.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID empty — not starting STA (set via NVS or sdkconfig). Skipping Wi-Fi.");
        ESP_LOGW(TAG, "To configure: nvs_set_str gateway/wifi_ssid and gateway/wifi_pass then reboot.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initializing Wi-Fi SSID='%s' (pass %s)", cfg.wifi_ssid, cfg.wifi_password[0] ? "***" : "(open)");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wificfg = {0};
    strncpy((char*)wificfg.sta.ssid, cfg.wifi_ssid, sizeof(wificfg.sta.ssid)-1);
    strncpy((char*)wificfg.sta.password, cfg.wifi_password, sizeof(wificfg.sta.password)-1);
    wificfg.sta.threshold.authmode = cfg.wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wificfg.sta.pmf_cfg.capable = true;
    wificfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wificfg));

    // reconnect timer
    esp_timer_create_args_t targs = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_start());
    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi started, waiting for events");
    return ESP_OK;
}

bool wifi_manager_is_connected(void) { return s_connected; }

esp_err_t wifi_manager_get_ip(char *out, size_t len) {
    if (!out || len==0) return ESP_ERR_INVALID_ARG;
    strncpy(out, s_ip_str, len-1);
    out[len-1]='\0';
    return ESP_OK;
}

int8_t wifi_manager_get_rssi(void) {
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info)==ESP_OK) return info.rssi;
    return 0;
}

uint32_t wifi_manager_get_disconnects(void) { return s_disconnects; }
