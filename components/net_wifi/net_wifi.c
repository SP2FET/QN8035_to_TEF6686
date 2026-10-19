#include "net_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

#define STA_RETRY_US (10 * 1000 * 1000)

static esp_timer_handle_t s_retry_timer;

static void retry_connect(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client " MACSTR " joined", MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "client " MACSTR " left", MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "station disconnected (reason %d), retry in %d s", e->reason,
                 STA_RETRY_US / 1000000);
        /* Back off instead of reconnecting at once: scanning moves the AP
         * channel and disturbs clients of the SoftAP. */
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, STA_RETRY_US);
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "station IP " IPSTR " -> http://" IPSTR "/", IP2STR(&e->ip_info.ip),
                 IP2STR(&e->ip_info.ip));
    }
}

esp_err_t net_wifi_start(const net_wifi_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->ap_ssid && cfg->ap_ssid[0], ESP_ERR_INVALID_ARG, TAG, "no AP SSID");
    const bool sta = cfg->sta_ssid && cfg->sta_ssid[0];
    const char *ap_pw = cfg->ap_password ? cfg->ap_password : "";

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap(), ESP_FAIL, TAG, "ap netif");
    if (sta) {
        ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta(), ESP_FAIL, TAG, "sta netif");
        const esp_timer_create_args_t targs = {.callback = retry_connect, .name = "wifi_retry"};
        ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "retry timer");
    }

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL),
                        TAG, "ip events");

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, cfg->ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen((const char *)ap.ap.ssid);
    ap.ap.channel = cfg->ap_channel;
    ap.ap.max_connection = 4;
    if (strlen(ap_pw) >= 8) {
        strlcpy((char *)ap.ap.password, ap_pw, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (ap_pw[0]) {
            ESP_LOGW(TAG, "AP password shorter than 8 characters, starting an open AP");
        }
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(sta ? WIFI_MODE_APSTA : WIFI_MODE_AP), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    if (sta) {
        wifi_config_t st = {0};
        strlcpy((char *)st.sta.ssid, cfg->sta_ssid, sizeof(st.sta.ssid));
        strlcpy((char *)st.sta.password, cfg->sta_password ? cfg->sta_password : "",
                sizeof(st.sta.password));
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &st), TAG, "sta config");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_LOGI(TAG, "AP \"%s\" (%s) on channel %u -> http://192.168.4.1/%s", cfg->ap_ssid,
             ap.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2", cfg->ap_channel,
             sta ? ", station enabled" : "");
    return ESP_OK;
}
