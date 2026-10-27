#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 20
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_commit(nvs_handle_t h);
