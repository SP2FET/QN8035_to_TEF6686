/**
 * @file
 * Wi-Fi bring-up: a SoftAP is always started (the car has no network), and
 * an optional station connection when an SSID is configured.
 * Requires NVS to be initialised by the caller.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ap_ssid;
    const char *ap_password; /**< < 8 characters: open network */
    uint8_t ap_channel;
    const char *sta_ssid;    /**< NULL or "" disables station mode */
    const char *sta_password;
    uint8_t tx_power_dbm;    /**< 2..20; 0 keeps the driver's maximum (20 dBm) */
    /** A client with the DHCP lease `ip` (IPv4, network byte order) left the SoftAP. Runs in the
     *  default event task: only signal. Not called for a client without a lease. May be NULL. */
    void (*on_client_left)(uint32_t ip);
} net_wifi_config_t;

esp_err_t net_wifi_start(const net_wifi_config_t *cfg);

#ifdef __cplusplus
}
#endif
