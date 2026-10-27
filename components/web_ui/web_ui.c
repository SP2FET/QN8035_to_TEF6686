#include "web_ui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "af_store.h"
#include "log_stream.h"
#include "net_wifi.h"
#include "qn8035_slave.h"
#include "qn_bridge.h"
#include "radio.h"
#include "radio_mem.h"
#include "sysmon.h"
#include "tuning.h"
#include "tuning_store.h"
#include "web_ota.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* ~2 kB plus up to AF_MAX entries of ~220 bytes and RADIO_AF_EVENTS of ~130. */
#define STATUS_BUF_SIZE 12288 /* and the reset history, SYSMON_HIST_MAX entries of ~70 */
/* /api/trace: records are copied in batches and sent in chunks; one record is < 300 bytes. */
#define TRACE_BATCH      16
#define TRACE_CHUNK      1024
#define TRACE_CHUNK_FILL 640
#define COREDUMP_CHUNK  1024
/* /api/tuning goes out in chunks; one parameter is < 300 bytes. */
#define TUNING_CHUNK      1024
#define TUNING_CHUNK_FILL 640
/* Per used channel: ~48 bytes of keys and numbers plus the name, escaped at worst 2x. */
#define PRESETS_BUF_SIZE (96 + PRESETS_MAX * (48 + 2 * PRESET_NAME_SIZE))
#define QUERY_MAX       128
#define NAME_PARAM_SIZE (3 * PRESET_NAME_SIZE) /* every byte percent-encoded */
#define LOG_CHUNK       1024
#define MAX_CLIENTS     8
#define PUMP_PERIOD_MS  100
#define PUMP_MAX_CHUNKS 4 /* per period */
/* Chunks queued to httpd and not yet sent. httpd_queue_work posts to a UDP control socket
 * whose mailbox (LWIP_UDP_RECVMBOX_SIZE, 6) drops what does not fit without an error, and a
 * dropped chunk is never freed: stay below it. */
#define PUMP_MAX_QUEUED 4
/* Back-off of the httpd task after a failed select(), doubling while it keeps failing. */
#define SELECT_BACKOFF_MIN_MS 50
#define SELECT_BACKOFF_MAX_MS 1000
#define SELECT_LOG_MS         10000

static httpd_handle_t s_server;
static uint32_t s_conn_resets; /* httpd task only */
static uint32_t s_send_resets; /* httpd task only */

/*
 * Log pump. Everything before s_pump_pos has been broadcast (to whoever was
 * connected). A new client is put on the pending list and gets the history
 * [oldest, chunk.start) right before its first chunk, both from the httpd
 * task, so each byte is delivered exactly once.
 */
static uint32_t s_pump_pos;
static volatile bool s_history_wanted;
static int s_pending[MAX_CLIENTS]; /* httpd task only; 0 = empty slot */
static volatile uint32_t s_chunks_queued; /* written by log_pump only */
static volatile uint32_t s_chunks_done;   /* written by the httpd task only */

/* Failed select() calls in the httpd task; status_get reads them in the same task. */
static struct {
    uint32_t count; /* since boot */
    int err;        /* errno of the last one */
    uint32_t run;   /* in the current run of failures */
    uint32_t backoff_ms;
    int64_t run_start_us, last_us, logged_us;
} s_sel;

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

/* 0.1-unit value, or null when unknown. */
static void jb_tenths(jbuf_t *j, bool known, int v)
{
    if (known) {
        jb_printf(j, "%.1f", v / 10.0);
    } else {
        jb_printf(j, "null");
    }
}

/* 0.1-unit score or level, null for AF_SCORE_NONE. */
static void jb_score(jbuf_t *j, int16_t v)
{
    jb_tenths(j, v != AF_SCORE_NONE, v);
}

static const char *af_ev_name(uint8_t type)
{
    static const char *const names[] = {"switched", "other_pi", "no_pi", "jumped",
                                        "check",    "trial",    "worse", "abort",
                                        "search",   "gave_up"};
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "?";
}

/* The event names a PI (radio_af_event_t.pi). */
static bool af_ev_pi(uint8_t type)
{
    return type == RADIO_AF_EV_OTHER_PI || type == RADIO_AF_EV_SEARCH ||
           type == RADIO_AF_EV_GAVE_UP;
}

/* One AF as measured by AF_Update, and its last trial. */
static void jb_af_entry(jbuf_t *j, const af_entry_t *e, bool lists, uint32_t now_ms)
{
    const bool banned = e->banned && (int32_t)(e->banned_until_ms - now_ms) > 0;
    const bool listed = e->listed && (uint32_t)(now_ms - e->listed_ms) < AF_TRUST_MS;
    const uint16_t usn = af_usn_min(e);
    const bool waiting = e->losses > 0 && (int32_t)(e->retry_until_ms - now_ms) > 0;
    jb_printf(j, "{\"khz\":%" PRIu32 ",\"score\":", e->khz);
    jb_score(j, e->score);
    jb_printf(j, ",\"last\":");
    jb_tenths(j, e->measured && e->last != AF_SCORE_NONE, e->last);
    jb_printf(j, ",\"n\":%u,\"streak\":%u,\"level\":", e->avg.n, e->streak);
    jb_tenths(j, e->measured, e->level);
    jb_printf(j, ",\"lavg\":");
    jb_score(j, e->level_avg);
    jb_printf(j, ",\"heard\":%s,\"listed\":%s,\"usn\":", b(e->heard), b(listed));
    jb_tenths(j, usn != UINT16_MAX, usn);
    jb_printf(j, ",\"trial\":");
    jb_score(j, e->trial_score);
    jb_printf(j, ",\"losses\":%u,\"retry_s\":%" PRIu32 ",\"age_s\":", e->losses,
              waiting ? (e->retry_until_ms - now_ms + 999) / 1000 : 0);
    if (e->measured) {
        jb_printf(j, "%" PRIu32, (now_ms - e->measured_ms) / 1000);
    } else {
        jb_printf(j, "null");
    }
    jb_printf(j, ",\"banned\":%s,\"ban_s\":%" PRIu32 ",\"trusted\":%s}", b(banned),
              banned ? (e->banned_until_ms - now_ms + 999) / 1000 : 0,
              b(af_entry_trusted(e, lists, now_ms)));
}

static void jb_af(jbuf_t *j, const radio_af_status_t *a)
{
    jb_printf(j, "\"af\":{\"active\":%s,\"poor\":%s,\"checking_pi\":%s,\"probation\":%s,\"pi\":",
              b(a->active), b(a->poor), b(a->checking_pi), b(a->probation));
    if (a->active) {
        jb_printf(j, "\"%04X\"", a->pi);
    } else {
        jb_printf(j, "null");
    }
    jb_printf(j, ",\"pi_mem\":%s,\"start\":%s,\"search\":%s,\"lists\":%s,\"score\":",
              b(a->pi_mem), b(a->start), b(a->search), b(a->lists));
    jb_score(j, a->score);
    jb_printf(j, ",\"level\":");
    jb_score(j, a->level);
    jb_printf(j, ",\"need\":");
    jb_score(j, a->need);
    jb_printf(j, ",\"trial\":");
    if (a->trial) {
        jb_printf(j, "{\"khz\":%" PRIu32 ",\"left_ms\":%" PRIu32 ",\"n\":%u,\"mean\":",
                  a->trial_khz, a->trial_left_ms, a->trial_n);
        jb_score(j, a->trial_mean);
        jb_printf(j, ",\"ref\":");
        jb_score(j, a->trial_ref);
        jb_printf(j, "}");
    } else {
        jb_printf(j, "null");
    }
    af_store_info_t m;
    af_store_get_info(&m);
    jb_printf(j,
              ",\"mem\":{\"channels\":%u,\"saving\":%s,\"pending\":%u,\"writes\":%" PRIu32
              ",\"errors\":%" PRIu32 "}",
              a->mem_progs, b(m.running), m.pending, m.writes, m.errors);
    jb_printf(j, ",\"dwell_s\":%" PRIu32 ",\"confirm\":%u,\"fast\":%s,\"switches\":%" PRIu32
                 ",\"pi_fails\":%" PRIu32 ",\"list\":[",
              (a->dwell_ms + 999) / 1000, (unsigned)a->confirm, b(a->fast), a->switches,
              a->pi_fails);
    for (unsigned i = 0; i < a->n && i < AF_MAX; i++) {
        jb_printf(j, "%s", i ? "," : "");
        jb_af_entry(j, &a->e[i], a->lists, a->now_ms);
    }
    jb_printf(j, "],\"events\":[");
    for (unsigned i = 0; i < a->ev_n && i < RADIO_AF_EVENTS; i++) {
        const radio_af_event_t *ev = &a->ev[i];
        jb_printf(j,
                  "%s{\"type\":\"%s\",\"age_s\":%" PRIu32 ",\"from\":%" PRIu32 ",\"to\":%" PRIu32
                  ",\"score\":",
                  i ? "," : "", af_ev_name(ev->type), (a->now_ms - ev->t_ms) / 1000, ev->from_khz,
                  ev->to_khz);
        jb_score(j, ev->score);
        jb_printf(j, ",\"cur\":");
        jb_score(j, ev->cur);
        jb_printf(j, ",\"pi\":");
        if (af_ev_pi(ev->type)) {
            jb_printf(j, "\"%04X\"", ev->pi);
        } else {
            jb_printf(j, "null");
        }
        jb_printf(j, ",\"ban_s\":%" PRIu32 "}", ev->ban_s);
    }
    jb_printf(j, "]},");
}

/* Least free stack each task has had (bytes on ESP-IDF): a shrinking margin shows before an
 * overflow does. Tasks that do not exist in this build are left out. */
static void jb_stacks(jbuf_t *j)
{
    static const char *const names[] = {"radio", "qn_slave", "qn_trace", "sysmon", "af_store",
                                        "radio_mem", "log_pump", "httpd", "esp_timer", "tiT",
                                        "wifi", "IDLE"};
    jb_printf(j, "\"stacks\":{");
    const char *sep = "";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (t) {
            jb_printf(j, "%s\"%s\":%u", sep, names[i], (unsigned)uxTaskGetStackHighWaterMark(t));
            sep = ",";
        }
    }
    jb_printf(j, "},");
}

static const char *health_ev_name(uint8_t ev)
{
    switch (ev) {
    case RADIO_HEALTH_LOST:
        return "lost";
    case RADIO_HEALTH_READY:
        return "ready";
    case RADIO_HEALTH_BOOT_FAILED:
        return "boot_failed";
    default:
        return "?";
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    /* httpd runs all handlers in its one task, so static buffers are safe
     * and keep the 2 kB off its stack. */
    /* ~2.6 kB, from the heap once: the static DRAM segment is full. */
    static radio_status_t *st;
    static char buf[STATUS_BUF_SIZE];
    qn8035_slave_stats_t qn;
    if (st == NULL && (st = malloc(sizeof(*st))) == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    radio_get_status(st);
    qn8035_slave_get_stats(&qn);

    jbuf_t j = {.buf = buf, .size = sizeof(buf)};
    jb_printf(&j, "{\"uptime_s\":%" PRIu32 ",\"heap_free\":%" PRIu32 ",\"heap_min\":%" PRIu32
                  ",\"heap_largest\":%u,",
              (uint32_t)(esp_timer_get_time() / 1000000), esp_get_free_heap_size(),
              esp_get_minimum_free_heap_size(),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    /* elf_sha256 (first 8 bytes) lets scripts/ota_upload.py tell which build runs. */
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char elf_sha[17];
    /* The SDK string helper is capped by APP_RETRIEVE_LEN_ELF_SHA (default 9),
     * even with a larger destination. Read the descriptor to keep this API stable. */
    for (size_t i = 0; i < 8; i++) {
        snprintf(&elf_sha[2 * i], 3, "%02x", app->app_elf_sha256[i]);
    }
    jb_printf(&j, "\"fw\":{\"version\":");
    jb_str(&j, app->version);
    jb_printf(&j, ",\"built\":\"%s %s\",\"idf\":", app->date, app->time);
    jb_str(&j, app->idf_ver);
    jb_printf(&j, ",\"elf_sha256\":\"%s\",\"partition\":", elf_sha);
    jb_str(&j, running ? running->label : "");
    jb_printf(&j, ",\"ota_state\":\"%s\",\"ota_password\":%s},", web_ota_state_name(),
              b(web_ota_password_required()));

    sysmon_info_t sys;
    sysmon_get_info(&sys);
    jb_printf(&j, "\"sys\":{\"reset\":");
    jb_str(&j, sys.reset);
    jb_printf(&j, ",\"unexpected\":%s,\"warm\":%s,\"crashes\":%" PRIu32 ",\"resets\":%" PRIu32
                  ",\"history\":[",
              b(sys.unexpected), b(sys.warm), sys.crashes, sys.resets);
    for (unsigned i = 0; i < sys.hist_n && i < SYSMON_HIST_MAX; i++) {
        const sysmon_reset_t *h = &sys.hist[i];
        jb_printf(&j, "%s{\"reason\":", i ? "," : "");
        jb_str(&j, sysmon_reset_name(h->reason));
        jb_printf(&j, ",\"n\":%u,\"up_s\":%" PRIu32 ",\"wall\":", h->count, h->up_s);
        if (h->wall_s) {
            jb_printf(&j, "%" PRId64 "}", h->wall_s);
        } else {
            jb_printf(&j, "null}");
        }
    }
    jb_printf(&j, "],\"dump\":");
    if (sys.dump) {
        jb_printf(&j, "{\"task\":");
        jb_str(&j, sys.task);
        jb_printf(&j, ",\"elf\":");
        jb_str(&j, sys.elf);
        jb_printf(&j, ",\"pc\":\"0x%08" PRIx32 "\",\"bt\":[", sys.pc);
        for (unsigned i = 0; i < sys.bt_n; i++) {
            jb_printf(&j, "%s\"0x%08" PRIx32 "\"", i ? "," : "", sys.bt[i]);
        }
        jb_printf(&j, "],\"corrupted\":%s}", b(sys.bt_corrupted));
    } else {
        jb_printf(&j, "null");
    }
    jb_printf(&j, "},\"web\":{\"conn_resets\":%" PRIu32 ",\"send_resets\":%" PRIu32
                  ",\"select_errors\":%" PRIu32 ",\"select_errno\":%d},", s_conn_resets,
              s_send_resets, s_sel.count, s_sel.err);

    jb_printf(&j, "\"tef\":{\"ready\":%s,\"op_status\":%d,\"state\":", b(st->tef_ready),
              (int)st->op_state);
    jb_str(&j, st->tef_ready ? state_name(st->op_state) : "not ready");
    jb_printf(&j, ",\"device\":%u,\"hw\":%u,\"sw\":%u,\"variant\":", st->ident.device,
              st->ident.hw_version, st->ident.sw_version);
    jb_str(&j, st->ident.device ? tef6686_variant_name(st->ident.device) : "");
    jb_printf(&j, ",\"patch\":");
    jb_str(&j, st->patch ? st->patch : "none");
    jb_printf(&j, ",\"error\":");
    jb_str(&j, st->error);
    const radio_diag_t *dg = &st->diag;
    jb_printf(&j,
              ",\"diag\":{\"io_errors\":%" PRIu32 ",\"io_retries\":%" PRIu32
              ",\"recoveries\":%" PRIu32 ",\"boots\":%" PRIu32 ",\"boot_failures\":%" PRIu32
              ",\"last_recovery\":",
              dg->io_errors, dg->io_retries, dg->recoveries, dg->boots, dg->boot_failures);
    jb_str(&j, dg->last_recovery);
    if (dg->recoveries) {
        const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        jb_printf(&j, ",\"last_recovery_age_s\":%" PRIu32 "}",
                  (now_ms - dg->last_recovery_ms) / 1000);
    } else {
        jb_printf(&j, ",\"last_recovery_age_s\":null}");
    }
    jb_printf(&j, "},\"am_step_khz\":%" PRIu32 ",", st->am_step_khz);

    const bool fm = st->band == RADIO_BAND_FM;
    jb_printf(&j,
              "\"band\":\"%s\",\"freq_khz\":%" PRIu32 ",\"seeking\":%s,\"mute\":%s,"
              "\"volume_db\":%.1f,\"bw_auto\":%s,\"bw_khz\":%.1f,\"force_mono\":%s,"
              "\"clock_set\":%s,",
              fm ? "FM" : "AM", st->freq_khz, b(st->seeking), b(st->mute), st->volume / 10.0,
              b(st->bw_auto), st->bw / 10.0, b(st->force_mono), b(st->clock_set));

    const radio_quality_t *q = &st->q;
    jb_printf(&j, "\"q\":{\"valid\":%s,\"level\":%.1f,\"offset\":%.1f,\"bw\":%.1f,\"mod\":%.1f,",
              b(q->valid), q->level / 10.0, q->offset / 10.0, q->bandwidth / 10.0,
              q->modulation / 10.0);
    if (fm) {
        jb_printf(&j,
                  "\"usn\":%.1f,\"wam\":%.1f,\"stereo\":%s,\"dsp\":{\"softmute\":%.1f,"
                  "\"highcut\":%.1f,\"stereo\":%.1f,\"sthiblend\":%.1f}},",
                  q->usn / 10.0, q->wam / 10.0, b(q->stereo), q->softmute / 10.0,
                  q->highcut / 10.0, q->stblend / 10.0, q->sthiblend / 10.0);
    } else {
        jb_printf(&j, "\"usn\":null,\"wam\":null,\"stereo\":null,\"dsp\":null,\"noise\":%.1f},",
                  q->usn / 10.0);
    }
    jb_af(&j, &st->af);

    jb_printf(&j, "\"rds\":{\"sync\":%s,\"pi\":", b(st->rds_sync));
    if (st->rds.pi_valid) {
        jb_printf(&j, "\"%04X\"", st->rds.pi);
    } else {
        jb_printf(&j, "\"\"");
    }
    jb_printf(&j, ",\"ps\":");
    jb_str(&j, st->rds.ps);
    jb_printf(&j, ",\"rt\":");
    jb_str(&j, st->rds.rt);
    jb_printf(&j, ",\"groups\":%" PRIu32 ",\"errors\":%" PRIu32 ",\"pty\":", st->rds.groups,
              st->rds.rejected);
    if (st->rds.pty_valid) {
        jb_printf(&j, "%u", st->rds.pty);
    } else {
        jb_printf(&j, "null");
    }
    jb_printf(&j, ",\"tp\":%s,\"ta\":%s,\"ct\":", b(st->rds.tp), b(st->rds.ta));
    if (st->rds.ct.valid) {
        jb_printf(&j, "{\"unix\":%" PRId64 ",\"off_min\":%d}", rds_ct_unix(&st->rds.ct),
                  st->rds.ct.offset * 30);
    } else {
        jb_printf(&j, "null");
    }
    jb_printf(&j, "},");

    jb_printf(&j, "\"qn\":{\"enabled\":%s,\"writes\":%" PRIu32 ",\"reads\":%" PRIu32 ",\"last\":",
              b(qn.running), qn.writes, qn.reads);
    jb_str(&j, qn.last);
    jb_printf(&j, ",\"dropped\":%" PRIu32 ",\"command_drops\":%" PRIu32 ",\"reconciles\":%" PRIu32,
              qn.dropped, qn.command_drops, qn.reconciles);
    jb_printf(&j, "},");
    jb_stacks(&j);
    qn_bridge_stats_t br;
    qn_bridge_get_stats(&br);
    /* left open: the memory channels part closes it */
    jb_printf(&j,
              "\"bridge\":{\"tunes\":%" PRIu32 ",\"scans\":%" PRIu32 ",\"timeouts\":%" PRIu32
              ",\"settle_timeouts\":%" PRIu32 ",\"refused\":%" PRIu32 ",\"queue_retries\":%" PRIu32,
              br.tunes, br.scans, br.timeouts, br.settle_timeouts, br.refused, br.queue_retries);

    radio_mem_info_t mem;
    radio_mem_get(NULL, &mem);
    jb_printf(&j,
              "},\"mem\":{\"enabled\":%s,\"rev\":%" PRIu32 ",\"used\":%u,\"max\":%d,"
              "\"writes\":%" PRIu32 ",\"errors\":%" PRIu32 "}}",
              b(mem.enabled), mem.rev, mem.used, PRESETS_MAX, mem.writes, mem.errors);

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
                               : err == ESP_ERR_NOT_FOUND ? "404 Not Found"
                               : err == ESP_ERR_NO_MEM    ? "507 Insufficient Storage"
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

/* ---- reception tuning ---------------------------------------------------- */

static esp_err_t send_chunk(httpd_req_t *req, jbuf_t *j)
{
    const esp_err_t err = httpd_resp_send_chunk(req, j->buf, (ssize_t)j->len);
    j->len = 0;
    return err;
}

/* Integer in chip units, null for AF_SCORE_NONE. */
static void jb_int(jbuf_t *j, int16_t v)
{
    if (v == AF_SCORE_NONE) {
        jb_printf(j, "null");
    } else {
        jb_printf(j, "%d", v);
    }
}

/* One trace record, chip units (0.1 dBuV, 0.1 %, 0.1 kHz, 0.1 dB). */
static void jb_trace(jbuf_t *j, uint32_t seq, const radio_trace_t *r)
{
    jb_printf(j, "{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",", seq, r->t_ms);
    if (r->kind == RADIO_TRACE_QUALITY) {
        jb_printf(j,
                  "\"k\":\"q\",\"f\":%" PRIu32 ",\"age\":%u,\"lv\":%d,\"usn\":%u,\"wam\":%u,"
                  "\"off\":%d,\"bw\":%u,\"mod\":%u,\"st\":%d,\"sm\":%u,\"hc\":%u,\"sb\":%u,"
                  "\"hb\":%u,\"sync\":%d,\"g\":%" PRIu32 ",\"rej\":%" PRIu32 ",\"af\":%u,\"cur\":",
                  r->khz, r->q.age, r->q.level, r->q.usn, r->q.wam, r->q.offset, r->q.bandwidth,
                  r->q.modulation, r->q.stereo, r->q.softmute, r->q.highcut, r->q.stblend,
                  r->q.sthiblend, r->q.rds_sync, r->q.rds_groups, r->q.rds_rejected, r->q.af);
        jb_int(j, r->q.cur);
        jb_printf(j, ",\"need\":");
        jb_int(j, r->q.need);
    } else if (r->kind == RADIO_TRACE_AF_MEASURE) {
        jb_printf(j,
                  "\"k\":\"a\",\"f\":%" PRIu32 ",\"lv\":%d,\"usn\":%u,\"wam\":%u,\"off\":%d,"
                  "\"streak\":%u,\"sc\":",
                  r->khz, r->a.level, r->a.usn, r->a.wam, r->a.offset, r->a.streak);
        jb_int(j, r->a.score);
        jb_printf(j, ",\"avg\":");
        jb_int(j, r->a.avg);
        jb_printf(j, ",\"lavg\":");
        jb_int(j, r->a.level_avg);
        jb_printf(j, ",\"need\":");
        jb_int(j, r->a.need);
    } else if (r->kind == RADIO_TRACE_HEALTH) {
        jb_printf(j,
                  "\"k\":\"h\",\"f\":%" PRIu32 ",\"ev\":\"%s\",\"op\":%u,\"io_err\":%" PRIu32
                  ",\"retries\":%" PRIu32 ",\"rec\":%" PRIu32 ",\"why\":",
                  r->khz, health_ev_name(r->h.event), r->h.op_state, r->h.io_errors,
                  r->h.io_retries, r->h.recoveries);
        jb_str(j, r->h.why);
    } else {
        const radio_af_event_t *e = &r->e;
        jb_printf(j,
                  "\"k\":\"e\",\"ev\":\"%s\",\"from\":%" PRIu32 ",\"to\":%" PRIu32
                  ",\"ban_s\":%" PRIu32 ",\"sc\":",
                  af_ev_name(e->type), e->from_khz, e->to_khz, e->ban_s);
        jb_int(j, e->score);
        jb_printf(j, ",\"cur\":");
        jb_int(j, e->cur);
        if (af_ev_pi(e->type)) {
            jb_printf(j, ",\"pi\":\"%04X\"", e->pi);
        } else {
            jb_printf(j, ",\"pi\":null");
        }
    }
    jb_printf(j, "}");
}

/* ?since=N: the radio's trace from sequence number N on, for a PC recording a ride
 * (scripts/ride_log.py). "next" is the N to ask for next; "lost" counts records overwritten
 * before they were fetched; "boot" changes with every start of the firmware. */
static esp_err_t trace_get(httpd_req_t *req)
{
    static radio_trace_t recs[TRACE_BATCH]; /* httpd task only */
    static char buf[TRACE_CHUNK];
    static uint32_t boot;
    if (boot == 0) {
        boot = esp_random() | 1u;
    }
    long arg = 0;
    const uint32_t since = get_long(req, "since", &arg) && arg >= 0 ? (uint32_t)arg : 0;
    uint32_t first, next, now;
    size_t n = radio_get_trace(since, recs, TRACE_BATCH, &first, &next, &now);
    /* since within this run, and older than the oldest record kept */
    const uint32_t lost =
        (int32_t)(next - since) >= 0 && (int32_t)(first - since) > 0 ? first - since : 0;
    const uint32_t end = next;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    jbuf_t j = {.buf = buf, .size = sizeof(buf)};
    jb_printf(&j,
              "{\"boot\":\"%08" PRIx32 "\",\"now\":%" PRIu32 ",\"first\":%" PRIu32
              ",\"lost\":%" PRIu32 ",\"r\":[",
              boot, now, first, lost);
    uint32_t seq = first;
    const char *sep = "";
    while (n > 0) {
        for (size_t i = 0; i < n; i++, seq++) {
            jb_printf(&j, "%s", sep);
            jb_trace(&j, seq, &recs[i]);
            sep = ",";
            if (j.len > TRACE_CHUNK_FILL) {
                ESP_RETURN_ON_ERROR(send_chunk(req, &j), TAG, "trace chunk");
            }
        }
        if ((int32_t)(end - seq) <= 0) {
            break;
        }
        uint32_t at;
        n = radio_get_trace(seq, recs, TRACE_BATCH, &at, &next, &now);
        if (at != seq) {
            break; /* overwritten while sending: the next request reports the gap */
        }
    }
    jb_printf(&j, "],\"next\":%" PRIu32 "}", seq);
    ESP_RETURN_ON_ERROR(send_chunk(req, &j), TAG, "trace chunk");
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* The whole table with current (v) and saved (sv) values, in chip units; display = v / div. */
static esp_err_t tuning_get(httpd_req_t *req)
{
    static char buf[TUNING_CHUNK]; /* httpd task only */
    static tuning_t cur, saved;
    radio_get_tuning(&cur);
    tuning_store_saved(&saved);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    jbuf_t j = {.buf = buf, .size = sizeof(buf)};
    jb_printf(&j, "{\"saved\":%s,\"params\":[", b(memcmp(&cur, &saved, sizeof(cur)) == 0));
    const char *sep = "";
    for (int i = 0; i < TUNING_COUNT; i++) {
        const tuning_param_t *p = &tuning_params[i];
        if (p->hidden) {
            continue;
        }
        jb_printf(&j, "%s{\"k\":", sep);
        jb_str(&j, p->key);
        jb_printf(&j, ",\"g\":");
        jb_str(&j, p->group);
        jb_printf(&j, ",\"l\":");
        jb_str(&j, p->label);
        jb_printf(&j, ",\"u\":");
        jb_str(&j, p->unit ? p->unit : "");
        jb_printf(&j, ",\"c\":");
        jb_str(&j, p->choices ? p->choices : "");
        jb_printf(&j, ",\"div\":%u,\"min\":%d,\"max\":%d,\"def\":%d,\"v\":%d,\"sv\":%d}", p->div,
                  p->min, p->max, p->def, cur.v[i], saved.v[i]);
        sep = ",";
        if (j.len > TUNING_CHUNK_FILL) {
            ESP_RETURN_ON_ERROR(send_chunk(req, &j), TAG, "tuning chunk");
        }
    }
    jb_printf(&j, "]}");
    ESP_RETURN_ON_ERROR(send_chunk(req, &j), TAG, "tuning chunk");
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ?k=key&v=value, value in chip units (the v of /api/tuning). Applied live, not saved. */
static esp_err_t tuning_post(httpd_req_t *req)
{
    char key[TUNING_KEY_MAX + 1];
    long v;
    if (!get_param(req, "k", key, sizeof(key)) || !get_long(req, "v", &v)) {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    const int id = tuning_find(key);
    return reply(req, id < 0 ? ESP_ERR_NOT_FOUND : radio_set_tuning(id, v));
}

static esp_err_t tuning_save_post(httpd_req_t *req)
{
    static tuning_t t;
    const esp_err_t err = radio_wait_commands(2000);
    if (err != ESP_OK) {
        return reply(req, err);
    }
    radio_get_tuning(&t);
    ESP_LOGI(TAG, "saving reception tuning");
    return reply(req, tuning_store_save(&t));
}

static esp_err_t tuning_defaults_post(httpd_req_t *req)
{
    static tuning_t t;
    tuning_defaults(&t);
    ESP_LOGI(TAG, "reception tuning: defaults (not saved)");
    return reply(req, radio_set_tuning_all(&t));
}

static esp_err_t tuning_revert_post(httpd_req_t *req)
{
    static tuning_t t;
    tuning_store_saved(&t);
    ESP_LOGI(TAG, "reception tuning: back to the saved values");
    return reply(req, radio_set_tuning_all(&t));
}

/* ---- memory channels ----------------------------------------------------- */

static int hex_digit(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* In place: %XX and '+' (URLSearchParams encodes a space as '+'); bad escapes stay as typed. */
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        int hi, lo;
        if (*s == '%' && (hi = hex_digit(s[1])) >= 0 && (lo = hex_digit(s[2])) >= 0) {
            *out++ = (char)(hi << 4 | lo);
            s += 2;
        } else {
            *out++ = *s == '+' ? ' ' : *s;
        }
    }
    *out = '\0';
}

/* ?slot=1..PRESETS_MAX (the number shown) to a 0-based slot. With `optional`, an absent
 * slot or 0 gives -1. */
static bool get_slot(httpd_req_t *req, bool optional, int *slot)
{
    char val[16];
    char *end;
    *slot = -1;
    if (!get_param(req, "slot", val, sizeof(val))) {
        return optional;
    }
    const long v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < (optional ? 0 : 1) || v > PRESETS_MAX) {
        return false;
    }
    *slot = (int)v - 1;
    return true;
}

/* ?name=, decoded into buf (NAME_PARAM_SIZE); absent gives NULL. false: too long. The radio_mem
 * side sanitises it (presets_copy_name). */
static bool get_name(httpd_req_t *req, char *buf, size_t size, const char **name)
{
    char query[QUERY_MAX];
    *name = NULL;
    esp_err_t err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (err == ESP_OK) {
        err = httpd_query_key_value(query, "name", buf, size);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    url_decode(buf);
    *name = buf;
    return true;
}

static esp_err_t presets_get(httpd_req_t *req)
{
    static presets_t p; /* httpd task only, like status_get's buffers */
    static char buf[PRESETS_BUF_SIZE];
    radio_mem_info_t info;
    radio_mem_get(&p, &info);

    jbuf_t j = {.buf = buf, .size = sizeof(buf)};
    jb_printf(&j, "{\"enabled\":%s,\"rev\":%" PRIu32 ",\"max\":%d,\"slots\":[", b(info.enabled),
              info.rev, PRESETS_MAX);
    const char *sep = "";
    for (int i = 0; i < PRESETS_MAX; i++) {
        const preset_t *e = &p.slot[i];
        if (e->khz == 0) {
            continue;
        }
        jb_printf(&j, "%s{\"slot\":%d,\"band\":\"%s\",\"khz\":%" PRIu32 ",\"name\":", sep, i + 1,
                  e->band == RADIO_BAND_AM ? "AM" : "FM", e->khz);
        jb_str(&j, e->name);
        jb_printf(&j, "}");
        sep = ",";
    }
    jb_printf(&j, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, (ssize_t)j.len);
}

static esp_err_t preset_store_post(httpd_req_t *req)
{
    char buf[NAME_PARAM_SIZE];
    const char *name;
    int slot, stored = -1;
    if (!get_slot(req, true, &slot) || !get_name(req, buf, sizeof(buf), &name)) {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    const esp_err_t err = radio_mem_store(slot, name, &stored);
    if (err != ESP_OK) {
        return reply(req, err);
    }
    char out[32];
    snprintf(out, sizeof(out), "{\"ok\":true,\"slot\":%d}", stored + 1);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t preset_recall_post(httpd_req_t *req)
{
    int slot;
    return reply(req, get_slot(req, false, &slot) ? radio_mem_recall(slot) : ESP_ERR_INVALID_ARG);
}

static esp_err_t preset_rename_post(httpd_req_t *req)
{
    char buf[NAME_PARAM_SIZE];
    const char *name;
    int slot;
    if (!get_slot(req, false, &slot) || !get_name(req, buf, sizeof(buf), &name) || !name) {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    return reply(req, radio_mem_rename(slot, name));
}

static esp_err_t preset_clear_post(httpd_req_t *req)
{
    int slot;
    return reply(req, get_slot(req, false, &slot) ? radio_mem_erase(slot) : ESP_ERR_INVALID_ARG);
}

static esp_err_t preset_step_post(httpd_req_t *req)
{
    const int dir = get_dir(req);
    return reply(req, (dir == 1 || dir == -1) ? radio_mem_step(dir) : ESP_ERR_INVALID_ARG);
}

/* The raw core dump, for espcoredump.py (sysmon.h). */
static esp_err_t coredump_get(httpd_req_t *req)
{
    static uint8_t chunk[COREDUMP_CHUNK]; /* httpd task only */
    size_t size;
    if (sysmon_dump_size(&size) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no core dump stored");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    for (size_t off = 0; off < size;) {
        const size_t n = MIN(sizeof(chunk), size - off);
        esp_err_t err = sysmon_dump_read(off, chunk, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)chunk, (ssize_t)n);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "core dump download: %s", esp_err_to_name(err));
            return err; /* httpd drops the connection: the file is incomplete */
        }
        off += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* crc = 1..8 hex digits: the CRC-32 of the downloaded file. */
static esp_err_t coredump_erase_post(httpd_req_t *req)
{
    char val[12];
    char *end;
    if (!get_param(req, "crc", val, sizeof(val)) || !isxdigit((unsigned char)val[0]) ||
        strlen(val) > 8) {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    const unsigned long crc = strtoul(val, &end, 16);
    if (*end != '\0') {
        return reply(req, ESP_ERR_INVALID_ARG);
    }
    const esp_err_t err = sysmon_dump_erase((uint32_t)crc);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no core dump stored");
    }
    if (err == ESP_ERR_INVALID_CRC) { /* httpd_err_code_t has no 409 */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "CRC does not match the stored core dump: kept");
    }
    return reply(req, err);
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

/* A client that stopped acknowledging (out of Wi-Fi range) makes every send block the httpd task
 * for send_wait_timeout (5 s) until TCP gives up minutes later, and httpd keeps the socket after
 * a failed send: close it. */
static bool ws_send(int fd, const char *p, size_t n)
{
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)p,
        .len = n,
    };
    if (httpd_ws_send_frame_async(s_server, fd, &f) == ESP_OK) {
        return true;
    }
    httpd_sess_trigger_close(s_server, fd);
    return false;
}

static bool ws_send_history(int fd, uint32_t upto)
{
    static char buf[LOG_CHUNK]; /* httpd task only */
    uint32_t pos = log_stream_oldest();
    size_t n;
    while ((n = log_stream_read(&pos, upto, buf, sizeof(buf))) > 0) {
        if (!ws_send(fd, buf, n)) {
            return false;
        }
    }
    return true;
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
            if (take_pending(fds[i]) && !ws_send_history(fds[i], c->start)) {
                continue;
            }
            if (c->len) {
                ws_send(fds[i], c->data, c->len);
            }
        }
    }
    memset(s_pending, 0, sizeof(s_pending)); /* leftovers are closed sockets */
    free(c);
    s_chunks_done++;
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
            if (s_chunks_queued - s_chunks_done >= PUMP_MAX_QUEUED) {
                break; /* httpd is behind; the ring keeps the lines (the oldest may be lost) */
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
            s_chunks_queued++; /* before: httpd may run the work and count it done at once */
            if (httpd_queue_work(s_server, broadcast_work, c) != ESP_OK) {
                s_chunks_queued--;
                free(c);
                break;
            }
        }
    }
}

/* ---- connections of departed clients ------------------------------------- */

/* httpd's socket is IPv6 when lwIP has it: IPv4 peers show as v4-mapped addresses. */
static bool peer_ipv4(int fd, uint32_t *ip)
{
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &len) != 0) {
        return false;
    }
    if (sa.ss_family == AF_INET) {
        *ip = ((const struct sockaddr_in *)&sa)->sin_addr.s_addr;
        return true;
    }
#if CONFIG_LWIP_IPV6
    static const uint8_t v4_mapped[12] = {[10] = 0xff, [11] = 0xff};
    const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&sa;
    if (sa.ss_family == AF_INET6 && memcmp(s6->sin6_addr.s6_addr, v4_mapped, 12) == 0) {
        memcpy(ip, &s6->sin6_addr.s6_addr[12], sizeof(*ip));
        return true;
    }
#endif
    return false;
}

/* Addresses of clients that left (IPv4, network byte order; 0 = free slot): the event task
 * fills them, reset_departed_work empties them. */
#define LEFT_SLOTS 4
static portMUX_TYPE s_left_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_left_ips[LEFT_SLOTS];
static bool s_left_queued;

/*
 * A client that leaves the access point leaves its connections behind: lwIP retransmits their
 * unsent data for many minutes and keeps it in the heap. With ~20 kB free, Wi-Fi then cannot
 * allocate the frames of the next key handshake, and the phone reports a wrong password until a
 * reset. shutdown() with a zero linger is lwIP's abortive close: it sends RST and frees the data
 * at once; httpd sees the socket fail and deletes the session. A client already back (same
 * lease) loses its new connections too; the page polls again.
 */
static void reset_departed_work(void *arg)
{
    (void)arg;
    uint32_t left[LEFT_SLOTS];
    taskENTER_CRITICAL(&s_left_lock);
    memcpy(left, s_left_ips, sizeof(left));
    memset(s_left_ips, 0, sizeof(s_left_ips));
    s_left_queued = false;
    taskEXIT_CRITICAL(&s_left_lock);

    size_t n = MAX_CLIENTS;
    int fds[MAX_CLIENTS];
    if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t ip;
        if (!peer_ipv4(fds[i], &ip)) {
            continue;
        }
        bool gone = false;
        for (size_t k = 0; k < LEFT_SLOTS && !gone; k++) {
            gone = left[k] == ip;
        }
        if (!gone) {
            continue;
        }
        const struct linger now = {.l_onoff = 1, .l_linger = 0};
        if (setsockopt(fds[i], SOL_SOCKET, SO_LINGER, &now, sizeof(now)) != 0) {
            ESP_LOGW(TAG, "SO_LINGER: %s (CONFIG_LWIP_SO_LINGER off?)", strerror(errno));
        }
        shutdown(fds[i], SHUT_RDWR);
        s_conn_resets++;
        ESP_LOGI(TAG, "reset connection %d of a client that left", fds[i]);
    }
}

void web_ui_client_left(uint32_t ip)
{
    if (s_server == NULL || ip == 0) {
        return;
    }
    /* One queued scan covers every departure before it runs: the control socket's mailbox is
     * small (see PUMP_MAX_QUEUED). More than LEFT_SLOTS clients leaving at once lose the rest. */
    taskENTER_CRITICAL(&s_left_lock);
    for (size_t k = 0; k < LEFT_SLOTS; k++) {
        if (s_left_ips[k] == 0 || s_left_ips[k] == ip) {
            s_left_ips[k] = ip;
            break;
        }
    }
    const bool queue = !s_left_queued;
    s_left_queued = true;
    taskEXIT_CRITICAL(&s_left_lock);
    if (queue && httpd_queue_work(s_server, reset_departed_work, NULL) != ESP_OK) {
        taskENTER_CRITICAL(&s_left_lock);
        s_left_queued = false; /* the next departure queues it again */
        taskEXIT_CRITICAL(&s_left_lock);
    }
}

/*
 * A send that failed (client out of range or not acknowledging; httpd's 5 s send timeout) makes
 * httpd drop the session. A graceful close would leave lwIP retransmitting the unsent data for
 * minutes, holding heap Wi-Fi needs (ride 2026-10-27: 308 bytes free, the AP dropped the laptop).
 * A zero linger makes close() send RST and free it at once. Normal closes stay graceful, so a
 * page load is never cut short. All sends run in the httpd task.
 */
static bool s_send_failed[CONFIG_LWIP_MAX_SOCKETS];

static bool *send_failed_slot(int fd)
{
    const int i = fd - LWIP_SOCKET_OFFSET;
    return (i >= 0 && i < CONFIG_LWIP_MAX_SOCKETS) ? &s_send_failed[i] : NULL;
}

/* httpd_default_send and httpd_sock_err (httpd_txrx.c) plus the mark */
static int session_send(httpd_handle_t hd, int fd, const char *buf, size_t len, int flags)
{
    (void)hd;
    if (buf == NULL) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    const int ret = send(fd, buf, len, flags);
    if (ret >= 0) {
        return ret;
    }
    const int err = errno;
    ESP_LOGW(TAG, "send on connection %d: %s", fd, strerror(err));
    bool *mark = send_failed_slot(fd);
    if (mark) {
        *mark = true;
    }
    switch (err) {
    case EAGAIN:
    case EINTR:
        return HTTPD_SOCK_ERR_TIMEOUT;
    case EINVAL:
    case EBADF:
    case EFAULT:
    case ENOTSOCK:
        return HTTPD_SOCK_ERR_INVALID;
    default:
        return HTTPD_SOCK_ERR_FAIL;
    }
}

static esp_err_t session_open(httpd_handle_t hd, int fd)
{
    bool *mark = send_failed_slot(fd);
    if (mark) {
        *mark = false;
    }
    return httpd_sess_set_send_override(hd, fd, session_send);
}

static void session_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    bool *mark = send_failed_slot(fd);
    if (mark && *mark) {
        *mark = false;
        const struct linger now = {.l_onoff = 1, .l_linger = 0};
        if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &now, sizeof(now)) != 0) {
            ESP_LOGW(TAG, "SO_LINGER: %s (CONFIG_LWIP_SO_LINGER off?)", strerror(errno));
        }
        s_send_resets++;
        ESP_LOGI(TAG, "reset connection %d after a failed send", fd);
    }
    close(fd);
}

/* ---- httpd select() failures --------------------------------------------- */

/*
 * ESP-IDF 5.4.0's httpd_server() answers a failed select() only with httpd_sess_delete_invalid()
 * and selects again at once. While the error lasts (ENOMEM: esp_vfs_select allocates on every
 * call) the httpd task spins at priority 5, starves the idle task, and the task watchdog resets
 * the board (core dumps of 2026-10-26). It is that function's only caller, so the link wraps it
 * (CMakeLists.txt) to pause there. Check this when ESP-IDF changes.
 */
void __real_httpd_sess_delete_invalid(httpd_handle_t hd);
void __wrap_httpd_sess_delete_invalid(httpd_handle_t hd);

static bool client_closed_under_httpd(httpd_handle_t hd)
{
    size_t n = MAX_CLIENTS;
    int fds[MAX_CLIENTS];
    if (httpd_get_client_list(hd, &n, fds) != ESP_OK) {
        return true; /* let httpd look */
    }
    for (size_t i = 0; i < n; i++) {
        if (fcntl(fds[i], F_GETFD) == -1 && errno == EBADF) {
            return true;
        }
    }
    return false;
}

void __wrap_httpd_sess_delete_invalid(httpd_handle_t hd)
{
    const int err = errno; /* of select(); httpd has logged it at error level */
    const int64_t now = esp_timer_get_time();
    if (s_sel.run == 0 || now - s_sel.last_us > 2000LL * SELECT_BACKOFF_MAX_MS) {
        s_sel.run = 0;
        s_sel.run_start_us = now;
        s_sel.logged_us = now - 1000LL * SELECT_LOG_MS;
        s_sel.backoff_ms = SELECT_BACKOFF_MIN_MS;
    } else {
        s_sel.backoff_ms = MIN(2 * s_sel.backoff_ms, (uint32_t)SELECT_BACKOFF_MAX_MS);
    }
    s_sel.count++;
    s_sel.run++;
    s_sel.err = err;
    s_sel.last_us = now;
    if (now - s_sel.logged_us >= 1000LL * SELECT_LOG_MS) {
        s_sel.logged_us = now;
        ESP_LOGE(TAG, "httpd select() fails: %s, %" PRIu32 "x in %" PRId64 " s; heap %" PRIu32
                 " free, largest block %u, %" PRIu32 " log chunks queued",
                 strerror(err), s_sel.run, (now - s_sel.run_start_us) / 1000000,
                 esp_get_free_heap_size(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 s_chunks_queued - s_chunks_done);
    }
    /* httpd's scan also reports every free slot as "invalid socket -1": run it only with work. */
    if (client_closed_under_httpd(hd)) {
        __real_httpd_sess_delete_invalid(hd);
    }
    vTaskDelay(pdMS_TO_TICKS(s_sel.backoff_ms));
}

/* ---- server -------------------------------------------------------------- */

esp_err_t web_ui_start(const web_ui_config_t *ui_cfg)
{
    ESP_RETURN_ON_FALSE(ui_cfg, ESP_ERR_INVALID_ARG, TAG, "null config");
    ESP_RETURN_ON_FALSE(s_server == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 32;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.open_fn = session_open;
    cfg.close_fn = session_close;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/trace", .method = HTTP_GET, .handler = trace_get},
        {.uri = "/api/coredump", .method = HTTP_GET, .handler = coredump_get},
        {.uri = "/api/coredump/erase", .method = HTTP_POST, .handler = coredump_erase_post},
        {.uri = "/api/tune", .method = HTTP_POST, .handler = tune_post},
        {.uri = "/api/step", .method = HTTP_POST, .handler = step_post},
        {.uri = "/api/seek", .method = HTTP_POST, .handler = seek_post},
        {.uri = "/api/volume", .method = HTTP_POST, .handler = volume_post},
        {.uri = "/api/mute", .method = HTTP_POST, .handler = mute_post},
        {.uri = "/api/bandwidth", .method = HTTP_POST, .handler = bandwidth_post},
        {.uri = "/api/reinit", .method = HTTP_POST, .handler = reinit_post},
        {.uri = "/api/tuning", .method = HTTP_GET, .handler = tuning_get},
        {.uri = "/api/tuning", .method = HTTP_POST, .handler = tuning_post},
        {.uri = "/api/tuning/save", .method = HTTP_POST, .handler = tuning_save_post},
        {.uri = "/api/tuning/defaults", .method = HTTP_POST, .handler = tuning_defaults_post},
        {.uri = "/api/tuning/revert", .method = HTTP_POST, .handler = tuning_revert_post},
        {.uri = "/api/presets", .method = HTTP_GET, .handler = presets_get},
        {.uri = "/api/preset/store", .method = HTTP_POST, .handler = preset_store_post},
        {.uri = "/api/preset/recall", .method = HTTP_POST, .handler = preset_recall_post},
        {.uri = "/api/preset/rename", .method = HTTP_POST, .handler = preset_rename_post},
        {.uri = "/api/preset/clear", .method = HTTP_POST, .handler = preset_clear_post},
        {.uri = "/api/preset/step", .method = HTTP_POST, .handler = preset_step_post},
        {.uri = "/ws",.method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
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
