/**
 * @file
 * Firmware update over HTTP (private to web_ui). See web_ota.c.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/** Registers POST /api/ota. password: NULL or "" accepts uploads without one. */
esp_err_t web_ota_register(httpd_handle_t server, const char *password);

/** OTA state of the running image: "valid", "pending" (on probation), "none" (flashed over
 * USB, no OTA record), ... */
const char *web_ota_state_name(void);

bool web_ota_password_required(void);
