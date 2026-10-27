/*
 * POST /api/ota: the body is the raw app image (firmware.bin), Content-Length required.
 *
 * The image streams into the inactive OTA slot. Before the first write the app descriptor
 * is checked against the running project; esp_ota_end then verifies header, chip and the
 * appended SHA-256. Only a complete, verified image becomes the boot partition, then the
 * device restarts. With BOOTLOADER_APP_ROLLBACK_ENABLE the new image starts on probation
 * and main.c confirms it after it has run for a while; a reset before that boots the
 * previous image again. Runs in the httpd task, which serves nothing else meanwhile.
 */
#include "web_ota.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "ota";

#define OTA_BUF_SIZE         4096
#define OTA_RECV_RETRIES     3            /* consecutive recv timeouts (recv_wait_timeout each) */
#define OTA_TOTAL_US         (120LL * 1000000)
#define OTA_PROGRESS_US      (20LL * 1000000)
#define OTA_RESTART_DELAY_US (500 * 1000) /* lets the response reach the client */
#define OTA_PASSWORD_MAX     64
#define OTA_PASSWORD_HDR     "X-OTA-Password"
/* The app descriptor follows the image header and the first segment header. */
#define OTA_DESC_OFFSET      (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static char s_password[OTA_PASSWORD_MAX + 1];
static bool s_restarting;

static esp_err_t ota_reply(httpd_req_t *req, const char *status, const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}",
             strcmp(status, "200 OK") == 0 ? "true" : "false", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static bool password_ok(httpd_req_t *req)
{
    if (s_password[0] == '\0') {
        return true;
    }
    char got[OTA_PASSWORD_MAX + 1] = {0};
    if (httpd_req_get_hdr_value_str(req, OTA_PASSWORD_HDR, got, sizeof(got)) != ESP_OK) {
        return false;
    }
    /* Length-independent comparison time; the length itself is not secret. */
    const size_t n = strlen(s_password);
    unsigned diff = strlen(got) != n;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned)(s_password[i] ^ got[i]);
    }
    return diff == 0;
}

/* Reads exactly len bytes of the body. */
static bool recv_exact(httpd_req_t *req, char *buf, size_t len, int64_t deadline)
{
    unsigned timeouts = 0;
    const int64_t progress_deadline = esp_timer_get_time() + OTA_PROGRESS_US;
    for (size_t got = 0; got < len;) {
        const int64_t now = esp_timer_get_time();
        if (now >= deadline || now >= progress_deadline) {
            return false;
        }
        const int n = httpd_req_recv(req, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= OTA_RECV_RETRIES) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        timeouts = 0;
        got += (size_t)n;
    }
    return true;
}

/* Rejects images of another project before anything is written; NULL if acceptable. */
static const char *check_descriptor(const char *buf, size_t len)
{
    esp_app_desc_t desc;
    if (len < OTA_DESC_OFFSET + sizeof(desc)) {
        return "image too short";
    }
    memcpy(&desc, buf + OTA_DESC_OFFSET, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "not an ESP-IDF app image";
    }
    const esp_app_desc_t *self = esp_app_get_description();
    if (strncmp(desc.project_name, self->project_name, sizeof(desc.project_name)) != 0) {
        return "image is for another project";
    }
    ESP_LOGI(TAG, "image %.32s %.32s, built %.16s %.16s", desc.project_name, desc.version,
             desc.date, desc.time);
    return NULL;
}

/* Streams the body into the OTA slot; NULL on success, else the reason. */
static const char *receive_image(httpd_req_t *req, esp_ota_handle_t ota, size_t total)
{
    static char buf[OTA_BUF_SIZE]; /* httpd task only */
    const int64_t deadline = esp_timer_get_time() + OTA_TOTAL_US;
    unsigned next_pct = 25;
    for (size_t done = 0; done < total;) {
        const size_t n = MIN(sizeof(buf), total - done);
        if (!recv_exact(req, buf, n, deadline)) {
            return "upload interrupted or deadline exceeded";
        }
        if (done == 0) {
            const char *why = check_descriptor(buf, n);
            if (why) {
                return why;
            }
        }
        const esp_err_t err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            return err == ESP_ERR_OTA_VALIDATE_FAILED ? "not an app image" : esp_err_to_name(err);
        }
        done += n;
        if (done * 100 >= total * next_pct && next_pct < 100) {
            ESP_LOGI(TAG, "%u %%", next_pct);
            next_pct += 25;
        }
    }
    return NULL;
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_restart(void)
{
    s_restarting = true;
    const esp_timer_create_args_t args = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) != ESP_OK ||
        esp_timer_start_once(t, OTA_RESTART_DELAY_US) != ESP_OK) {
        esp_restart();
    }
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (s_restarting) {
        return ota_reply(req, "409 Conflict", "restart pending");
    }
    if (!password_ok(req)) {
        ESP_LOGW(TAG, "upload refused: wrong or missing " OTA_PASSWORD_HDR);
        return ota_reply(req, "403 Forbidden", "wrong or missing " OTA_PASSWORD_HDR);
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return ota_reply(req, "500 Internal Server Error", "no OTA slot");
    }
    const size_t total = req->content_len;
    if (total < OTA_DESC_OFFSET + sizeof(esp_app_desc_t) || total > part->size) {
        return ota_reply(req, "400 Bad Request", "image size does not fit the OTA slot");
    }

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        return ota_reply(req, "409 Conflict", "running firmware not confirmed yet, retry later");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return ota_reply(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)total, part->label);

    const char *why = receive_image(req, ota, total);
    if (why) {
        esp_ota_abort(ota);
    } else if ((err = esp_ota_end(ota)) != ESP_OK) { /* frees the handle either way */
        why = err == ESP_ERR_OTA_VALIDATE_FAILED ? "image verification failed"
                                                 : esp_err_to_name(err);
    } else if ((err = esp_ota_set_boot_partition(part)) != ESP_OK) {
        why = esp_err_to_name(err);
    }
    if (why) {
        ESP_LOGE(TAG, "update failed: %s", why);
        return ota_reply(req, "400 Bad Request", why);
    }

    ESP_LOGW(TAG, "update written to %s, restarting", part->label);
    schedule_restart();
    return ota_reply(req, "200 OK", "verified, restarting");
}

esp_err_t web_ota_register(httpd_handle_t server, const char *password)
{
    ESP_RETURN_ON_FALSE(password == NULL || strlen(password) <= OTA_PASSWORD_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "password longer than %d", OTA_PASSWORD_MAX);
    strlcpy(s_password, password ? password : "", sizeof(s_password));
    const httpd_uri_t uri = {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post};
    return httpd_register_uri_handler(server, &uri);
}

const char *web_ota_state_name(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) {
        return "none";
    }
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

bool web_ota_password_required(void)
{
    return s_password[0] != '\0';
}
