#include "web_ui.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_stream.h"
#include "qn8035_slave.h"
#include "radio.h"
#include "web_ota.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define STATUS_BUF_SIZE 2048
#define QUERY_MAX       128
#define LOG_CHUNK       1024
#define MAX_CLIENTS     8
#define PUMP_PERIOD_MS  100
#define PUMP_MAX_CHUNKS 4 /* per period, bounds memory queued to httpd */

static httpd_handle_t s_server;

/*
 * Log pump. Everything before s_pump_pos has been broadcast (to whoever was
 * connected). A new client is put on the pending list and gets the history
 * [oldest, chunk.start) right before its first chunk, both from the httpd
 * task, so each byte is delivered exactly once.
 */
static uint32_t s_pump_pos;
static volatile bool s_history_wanted;
static int s_pending[MAX_CLIENTS]; /* httpd task only; 0 = empty slot */

typedef struct {
    uint32_t start;
    size_t len;
    char data[];
} log_chunk_t;

/* ---- JSON ---------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t size;
    size_t len;
} jbuf_t;

static void jb_printf(jbuf_t *j, const char *fmt, ...)
{
    if (j->len >= j->size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(j->buf + j->len, j->size - j->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        j->len = MIN(j->size - 1, j->len + (size_t)n);
    }
}

/* Quoted, escaped, trailing spaces trimmed (PS/RT are space padded). */
static void jb_str(jbuf_t *j, const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }
    jb_printf(j, "\"");
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            jb_printf(j, "\\%c", c);
        } else if (c < 0x20) {
            jb_printf(j, "\\u%04x", c);
        } else {
            jb_printf(j, "%c", c);
        }
    }
    jb_printf(j, "\"");
}

static const char *b(bool v)
{
    return v ? "true" : "false";
}

static const char *state_name(tef6686_op_state_t st)
{
    switch (st) {
    case TEF6686_STATE_BOOT:    return "boot";
    case TEF6686_STATE_IDLE:    return "idle";
    case TEF6686_STATE_STANDBY: return "active standby";
    case TEF6686_STATE_FM:      return "active FM";
    case TEF6686_STATE_AM:      return "active AM";
    default:                    return "unknown";
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    /* httpd runs all handlers in its one task, so static buffers are safe
     * and keep the 2 kB off its stack. */
    static radio_status_t st;
    static char buf[STATUS_BUF_SIZE];
    qn8035_slave_stats_t qn;
    radio_get_status(&st);
    qn8035_slave_get_stats(&qn);

    jbuf_t j = {.buf = buf, .size = sizeof(buf)};
    jb_printf(&j, "{\"uptime_s\":%" PRIu32 ",\"heap_free\":%" PRIu32 ",",
              (uint32_t)(esp_timer_get_time() / 1000000), esp_get_free_heap_size());

    /* elf_sha256 (first 8 bytes) lets scripts/ota_upload.py tell which build runs. */
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char elf_sha[17];
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    jb_printf(&j, "\"fw\":{\"version\":");
    jb_str(&j, app->version);
    jb_printf(&j, ",\"built\":\"%s %s\",\"idf\":", app->date, app->time);
    jb_str(&j, app->idf_ver);
    jb_printf(&j, ",\"elf_sha256\":\"%s\",\"partition\":", elf_sha);
    jb_str(&j, running ? running->label : "");
    jb_printf(&j, ",\"ota_state\":\"%s\",\"ota_password\":%s},", web_ota_state_name(),
              b(web_ota_password_required()));

    jb_printf(&j, "\"tef\":{\"ready\":%s,\"op_status\":%d,\"state\":", b(st.tef_ready),
              (int)st.op_state);
    jb_str(&j, st.tef_ready ? state_name(st.op_state) : "not ready");
    jb_printf(&j, ",\"device\":%u,\"hw\":%u,\"sw\":%u,\"variant\":", st.ident.device,
              st.ident.hw_version, st.ident.sw_version);
    jb_str(&j, st.ident.device ? tef6686_variant_name(st.ident.device) : "");
    jb_printf(&j, ",\"patch\":");
    jb_str(&j, st.patch ? st.patch : "none");
    jb_printf(&j, ",\"error\":");
    jb_str(&j, st.error);
    jb_printf(&j, "},");

    const bool fm = st.band == RADIO_BAND_FM;
    jb_printf(&j,
              "\"band\":\"%s\",\"freq_khz\":%" PRIu32 ",\"seeking\":%s,\"mute\":%s,"
              "\"volume_db\":%.1f,\"bw_auto\":%s,\"bw_khz\":%.1f,",
              fm ? "FM" : "AM", st.freq_khz, b(st.seeking), b(st.mute), st.volume / 10.0,
              b(st.bw_auto), st.bw / 10.0);

    const radio_quality_t *q = &st.q;
    jb_printf(&j, "\"q\":{\"valid\":%s,\"level\":%.1f,\"offset\":%.1f,\"bw\":%.1f,\"mod\":%.1f,",
              b(q->valid), q->level / 10.0, q->offset / 10.0, q->bandwidth / 10.0,
              q->modulation / 10.0);
    if (fm) {
        jb_printf(&j, "\"usn\":%.1f,\"wam\":%.1f,\"stereo\":%s},", q->usn / 10.0, q->wam / 10.0,
                  b(q->stereo));
    } else {
        jb_printf(&j, "\"usn\":null,\"wam\":null,\"stereo\":null,\"noise\":%.1f},", q->usn / 10.0);
    }

    jb_printf(&j, "\"rds\":{\"sync\":%s,\"pi\":", b(st.rds_sync));
    if (st.rds.pi_valid) {
        jb_printf(&j, "\"%04X\"", st.rds.pi);
    } else {
        jb_printf(&j, "\"\"");
    }
    jb_printf(&j, ",\"ps\":");
    jb_str(&j, st.rds.ps);
    jb_printf(&j, ",\"rt\":");
    jb_str(&j, st.rds.rt);
    jb_printf(&j, ",\"groups\":%" PRIu32 ",\"errors\":%" PRIu32 "},", st.rds.groups,
              st.rds.rejected);

    jb_printf(&j, "\"qn\":{\"enabled\":%s,\"writes\":%" PRIu32 ",\"reads\":%" PRIu32 ",\"last\":",
              b(qn.running), qn.writes, qn.reads);
    jb_str(&j, qn.last);
    jb_printf(&j, "}}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, (ssize_t)j.len);
}

/* ---- commands ------------------------------------------------------------ */

static bool get_param(httpd_req_t *req, const char *key, char *val, size_t size)
{
    char query[QUERY_MAX];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, val, size) == ESP_OK;
}

static bool get_long(httpd_req_t *req, const char *key, long *out)
{
    char val[16];
    char *end;
    if (!get_param(req, key, val, sizeof(val))) {
        return false;
    }
    *out = strtol(val, &end, 10);
    return end != val && *end == '\0';
}

static bool get_float(httpd_req_t *req, const char *key, float *out)
{
    char val[16];
    char *end;
    if (!get_param(req, key, val, sizeof(val))) {
        return false;
    }
    *out = strtof(val, &end);
    return end != val && *end == '\0' && isfinite(*out);
}

/* "up" -> 1, "down" -> -1, "stop" -> 0; anything else -> 2 */
static int get_dir(httpd_req_t *req)
{
    char val[8];
    if (!get_param(req, "dir", val, sizeof(val))) {
        return 2;
    }
    return !strcmp(val, "up") ? 1 : !strcmp(val, "down") ? -1 : !strcmp(val, "stop") ? 0 : 2;
}

static esp_err_t reply(httpd_req_t *req, esp_err_t err)
{
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
    httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request"
                                                          : "503 Service Unavailable");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t tune_post(httpd_req_t *req)
{
    char band[4];
    long khz;
    if (!get_param(req, "band", band, sizeof(band)) || !get_long(req, "khz", &khz) || khz <= 0) {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    radio_band_t rb;
    if (!strcasecmp(band, "FM")) {
        rb = RADIO_BAND_FM;
    } else if (!strcasecmp(band, "AM")) {
        rb = RADIO_BAND_AM;
    } else {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    return reply(req, radio_tune(rb, (uint32_t)khz));
}

static esp_err_t step_post(httpd_req_t *req)
{
    const int dir = get_dir(req);
    return reply(req, (dir == 1 || dir == -1) ? radio_step(dir) : ESP_ERR_INVALID_ARG);
}

static esp_err_t seek_post(httpd_req_t *req)
{
    const int dir = get_dir(req);
    return reply(req, dir != 2 ? radio_seek(dir) : ESP_ERR_INVALID_ARG);
}

static esp_err_t volume_post(httpd_req_t *req)
{
    float db;
    if (!get_float(req, "db", &db) || db < -60.0f || db > 24.0f) {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    return reply(req, radio_set_volume((int16_t)MAX(-599, lroundf(db * 10.0f))));
}

static esp_err_t mute_post(httpd_req_t *req)
{
    long on;
    if (!get_long(req, "on", &on) || (on != 0 && on != 1)) {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    return reply(req, radio_set_mute(on == 1));
}

static esp_err_t bandwidth_post(httpd_req_t *req)
{
    float khz;
    if (!get_float(req, "khz", &khz) || khz < 0.0f || khz > 400.0f) {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    return reply(req, radio_set_bandwidth(khz == 0.0f, (uint16_t)lroundf(khz * 10.0f)));
}

static esp_err_t reinit_post(httpd_req_t *req)
{
    ESP_LOGI(TAG, "tuner re-init requested from web");
    return reply(req, radio_reinit());
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* EMBED_TXTFILES appends a NUL terminator */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- WebSocket log console ---------------------------------------------- */

static void ws_send(int fd, const char *p, size_t n)
{
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)p,
        .len = n,
    };
    httpd_ws_send_frame_async(s_server, fd, &f); /* httpd drops broken sockets */
}

static void ws_send_history(int fd, uint32_t upto)
{
    static char buf[LOG_CHUNK]; /* httpd task only */
    uint32_t pos = log_stream_oldest();
    size_t n;
    while ((n = log_stream_read(&pos, upto, buf, sizeof(buf))) > 0) {
        ws_send(fd, buf, n);
    }
}

static bool take_pending(int fd)
{
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (s_pending[i] == fd) {
            s_pending[i] = 0;
            return true;
        }
    }
    return false;
}

static void broadcast_work(void *arg)
{
    log_chunk_t *c = arg;
    size_t n = MAX_CLIENTS;
    int fds[MAX_CLIENTS];

    if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
                continue;
            }
            if (take_pending(fds[i])) {
                ws_send_history(fds[i], c->start);
            }
            if (c->len) {
                ws_send(fds[i], c->data, c->len);
            }
        }
    }
    memset(s_pending, 0, sizeof(s_pending)); /* leftovers are closed sockets */
    free(c);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) { /* handshake complete */
        const int fd = httpd_req_to_sockfd(req);
        for (size_t i = 0; i < MAX_CLIENTS; i++) {
            if (s_pending[i] == 0) {
                s_pending[i] = fd;
                break;
            }
        }
        s_history_wanted = true;
        return ESP_OK;
    }
    /* Clients have nothing to say; read and drop whatever arrives. */
    uint8_t tmp[64];
    httpd_ws_frame_t f = {.payload = tmp};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err == ESP_OK && f.len > 0) {
        ESP_RETURN_ON_FALSE(f.len <= sizeof(tmp), ESP_FAIL, TAG, "ws frame too large");
        err = httpd_ws_recv_frame(req, &f, sizeof(tmp));
    }
    return err;
}

static void log_pump_task(void *arg)
{
    (void)arg;
    s_pump_pos = log_stream_head();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PUMP_PERIOD_MS));
        for (int i = 0; i < PUMP_MAX_CHUNKS; i++) {
            const uint32_t head = log_stream_head();
            if (head == s_pump_pos && !s_history_wanted) {
                break;
            }
            const size_t want = MIN((size_t)(head - s_pump_pos), (size_t)LOG_CHUNK);
            log_chunk_t *c = malloc(sizeof(*c) + want);
            if (c == NULL) {
                break;
            }
            uint32_t pos = s_pump_pos;
            c->len = log_stream_read(&pos, head, c->data, want);
            c->start = pos - (uint32_t)c->len;
            s_pump_pos = pos;
            s_history_wanted = false;
            if (httpd_queue_work(s_server, broadcast_work, c) != ESP_OK) {
                free(c);
                break;
            }
        }
    }
}

/* ---- server -------------------------------------------------------------- */

esp_err_t web_ui_start(const web_ui_config_t *ui_cfg)
{
    ESP_RETURN_ON_FALSE(ui_cfg, ESP_ERR_INVALID_ARG, TAG, "null config");
    ESP_RETURN_ON_FALSE(s_server == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/tune", .method = HTTP_POST, .handler = tune_post},
        {.uri = "/api/step", .method = HTTP_POST, .handler = step_post},
        {.uri = "/api/seek", .method = HTTP_POST, .handler = seek_post},
        {.uri = "/api/volume", .method = HTTP_POST, .handler = volume_post},
        {.uri = "/api/mute", .method = HTTP_POST, .handler = mute_post},
        {.uri = "/api/bandwidth", .method = HTTP_POST, .handler = bandwidth_post},
        {.uri = "/api/reinit", .method = HTTP_POST, .handler = reinit_post},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &uris[i]), TAG, "register %s",
                            uris[i].uri);
    }
    ESP_RETURN_ON_ERROR(web_ota_register(s_server, ui_cfg->ota_password), TAG, "register ota");

    ESP_RETURN_ON_FALSE(xTaskCreate(log_pump_task, "log_pump", 3072, NULL, 3, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "pump task");
    ESP_LOGI(TAG, "web UI on port %d", cfg.server_port);
    return ESP_OK;
}
