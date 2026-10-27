#include "net_wifi.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

#define STA_RETRY_US (10 * 1000 * 1000)

static esp_timer_handle_t s_retry_timer;
static esp_netif_t *s_ap_netif;
static void (*s_on_client_left)(uint32_t ip);

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
    /* The free heap goes with both: Wi-Fi allocates every received frame and the key handshake's
     * frames from it, and with none left a phone reports a wrong password (reason 15, handshake
     * timeout). */
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client " MACSTR " joined; heap %" PRIu32 " free, largest block %u",
                 MAC2STR(e->mac), esp_get_free_heap_size(),
                 heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *e = data;
        /* The lease outlives the association (a client coming back gets the same address). */
        esp_netif_pair_mac_ip_t lease = {.ip.addr = 0};
        memcpy(lease.mac, e->mac, sizeof(lease.mac));
        if (esp_netif_dhcps_get_clients_by_mac(s_ap_netif, 1, &lease) != ESP_OK) {
            lease.ip.addr = 0;
        }
        ESP_LOGI(TAG, "client " MACSTR " (" IPSTR ") left (reason %u); heap %" PRIu32
                 " free, largest block %u", MAC2STR(e->mac), IP2STR(&lease.ip), e->reason,
                 esp_get_free_heap_size(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        if (s_on_client_left && lease.ip.addr != 0) {
            s_on_client_left(lease.ip.addr);
        }
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
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif, ESP_FAIL, TAG, "ap netif");
    s_on_client_left = cfg->on_client_left;
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
    if (cfg->tx_power_dbm) {
        /* 0.25 dBm units, 8..84 = 2..20 dBm (only after esp_wifi_start). Transmit bursts at
         * 20 dBm draw ~300 mA, which a weak 3.3 V supply does not hold. */
        unsigned dbm = cfg->tx_power_dbm > 20 ? 20 : cfg->tx_power_dbm;
        dbm = dbm < 2 ? 2 : dbm;
        ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power((int8_t)(dbm * 4)), TAG, "tx power");
    }

    ESP_LOGI(TAG, "AP \"%s\" (%s) on channel %u, %u dBm -> http://192.168.4.1/%s", cfg->ap_ssid,
             ap.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2", cfg->ap_channel,
             cfg->tx_power_dbm ? cfg->tx_power_dbm : 20, sta ? ", station enabled" : "");
    return ESP_OK;
}
