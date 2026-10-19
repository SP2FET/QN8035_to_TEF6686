#include "qn8035_slave.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "driver/i2c_slave.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2
#error "qn8035_slave needs CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2=y (see sdkconfig.defaults)"
#endif

static const char *TAG = "qn8035";

#define RX_MAX          32   /* one hardware FIFO */
#define MSG_QUEUE_LEN   16
#define TRACE_QUEUE_LEN 32
#define TRACE_BYTES     8
#define TX_TIMEOUT_MS   10
#define INT_PULSE_US    4500 /* datasheet: 4.5 ms low pulse */
#define SLAVE_TASK_PRIO (configMAX_PRIORITIES - 2)
#define TRACE_TASK_PRIO 2

typedef enum {
    MSG_WRITE,
    MSG_READ,
} msg_type_t;

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t data[RX_MAX];
} bus_msg_t;

typedef struct {
    uint8_t type;
    uint8_t reg;
    uint8_t len; /* data bytes, may exceed TRACE_BYTES */
    uint8_t data[TRACE_BYTES];
} trace_rec_t;

static struct {
    qn8035_emu_t emu;
    SemaphoreHandle_t lock; /* guards emu and stats */
    QueueHandle_t msgs;
    QueueHandle_t traces;
    i2c_slave_dev_handle_t dev;
    gpio_num_t int_gpio;
    esp_timer_handle_t int_timer;
    qn8035_slave_event_cb_t cb;
    void *cb_ctx;
    bool trace;
    volatile uint32_t isr_dropped;
    qn8035_slave_stats_t stats;
} s;

/* ---- ISR side ------------------------------------------------------------- */

static bool on_receive(i2c_slave_dev_handle_t dev, const i2c_slave_rx_done_event_data_t *evt,
                       void *arg)
{
    (void)dev;
    (void)arg;
    bus_msg_t m = {.type = MSG_WRITE, .len = (uint8_t)MIN(evt->length, (uint32_t)RX_MAX)};
    memcpy(m.data, evt->buffer, m.len);
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s.msgs, &m, &woken) != pdTRUE) {
        s.isr_dropped++;
    }
    return woken == pdTRUE;
}

static bool on_request(i2c_slave_dev_handle_t dev, const i2c_slave_request_event_data_t *evt,
                       void *arg)
{
    (void)dev;
    (void)evt;
    (void)arg;
    bus_msg_t m = {.type = MSG_READ};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s.msgs, &m, &woken) != pdTRUE) {
        s.isr_dropped++;
    }
    return woken == pdTRUE;
}

/* ---- INT pin -------------------------------------------------------------- */

static void int_release(void *arg)
{
    (void)arg;
    gpio_set_level(s.int_gpio, 1);
}

static void pulse_int(void)
{
    if (s.int_gpio == GPIO_NUM_NC) {
        return;
    }
    gpio_set_level(s.int_gpio, 0);
    esp_timer_stop(s.int_timer); /* restart the pulse if one is running */
    esp_timer_start_once(s.int_timer, INT_PULSE_US);
}

/* ---- tasks ---------------------------------------------------------------- */

static void trace(uint8_t type, uint8_t reg, const uint8_t *data, size_t len)
{
    trace_rec_t rec = {.type = type, .reg = reg, .len = (uint8_t)len};
    memcpy(rec.data, data, MIN(len, sizeof(rec.data)));
    if (xQueueSend(s.traces, &rec, 0) != pdTRUE) {
        s.isr_dropped++; /* counted together; both mean lost visibility */
    }
}

static void slave_task(void *arg)
{
    (void)arg;
    bus_msg_t m;
    qn8035_evt_t evts[QN8035_EMU_MAX_EVENTS];

    for (;;) {
        xQueueReceive(s.msgs, &m, portMAX_DELAY);
        if (m.type == MSG_READ) {
            uint8_t b;
            xSemaphoreTake(s.lock, portMAX_DELAY);
            const uint8_t reg = s.emu.ptr;
            qn8035_emu_read(&s.emu, &b, 1);
            s.stats.reads++;
            xSemaphoreGive(s.lock);
            /* One byte per request: the known drivers read single registers.
             * TODO(bring-up): confirm on the logic analyser what a multi-byte
             * read does with this driver (extra on_request or 0xFF). */
            uint32_t written = 0;
            i2c_slave_write(s.dev, &b, 1, &written, TX_TIMEOUT_MS);
            trace(MSG_READ, reg, &b, 1);
            continue;
        }
        if (m.len == 0) {
            continue;
        }
        xSemaphoreTake(s.lock, portMAX_DELAY);
        const size_t n = qn8035_emu_write(&s.emu, m.data, m.len, evts, QN8035_EMU_MAX_EVENTS);
        s.stats.writes++;
        xSemaphoreGive(s.lock);
        trace(MSG_WRITE, m.data[0], &m.data[1], m.len - 1u);
        for (size_t i = 0; i < n; i++) {
            s.cb(&evts[i], s.cb_ctx);
        }
    }
}

static void trace_task(void *arg)
{
    (void)arg;
    trace_rec_t rec;
    char hex[3 * TRACE_BYTES + 4];
    char line[sizeof(((qn8035_slave_stats_t *)0)->last)];

    for (;;) {
        xQueueReceive(s.traces, &rec, portMAX_DELAY);
        size_t pos = 0;
        hex[0] = '\0';
        for (size_t i = 0; i < MIN((size_t)rec.len, (size_t)TRACE_BYTES); i++) {
            pos += (size_t)snprintf(&hex[pos], sizeof(hex) - pos, "%s%02x", i ? " " : "", rec.data[i]);
        }
        if (rec.len > TRACE_BYTES) {
            strlcat(hex, " ..", sizeof(hex));
        }
        snprintf(line, sizeof(line), "%c %02x %s %s", rec.type == MSG_WRITE ? 'W' : 'R', rec.reg,
                 rec.type == MSG_WRITE ? "<-" : "->", rec.len ? hex : "(addr only)");
        xSemaphoreTake(s.lock, portMAX_DELAY);
        strlcpy(s.stats.last, line, sizeof(s.stats.last));
        xSemaphoreGive(s.lock);
        if (s.trace) {
            ESP_LOGI(TAG, "%s", line);
        }
    }
}

/* ---- public API ----------------------------------------------------------- */

esp_err_t qn8035_slave_start(const qn8035_slave_config_t *cfg, qn8035_slave_event_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cfg && cb, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    ESP_RETURN_ON_FALSE(!s.stats.running, ESP_ERR_INVALID_STATE, TAG, "already running");

    qn8035_emu_init(&s.emu);
    s.cb = cb;
    s.cb_ctx = ctx;
    s.trace = cfg->trace;
    s.int_gpio = cfg->int_gpio;
    s.lock = xSemaphoreCreateMutex();
    s.msgs = xQueueCreate(MSG_QUEUE_LEN, sizeof(bus_msg_t));
    s.traces = xQueueCreate(TRACE_QUEUE_LEN, sizeof(trace_rec_t));
    ESP_RETURN_ON_FALSE(s.lock && s.msgs && s.traces, ESP_ERR_NO_MEM, TAG, "rtos objects");

    if (cfg->int_gpio != GPIO_NUM_NC) {
        const gpio_config_t io = {.pin_bit_mask = 1ULL << cfg->int_gpio, .mode = GPIO_MODE_OUTPUT};
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "INT gpio");
        gpio_set_level(cfg->int_gpio, 1);
        const esp_timer_create_args_t targs = {.callback = int_release, .name = "qn_int"};
        ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s.int_timer), TAG, "INT timer");
    }

    /* Tasks first so nothing the head unit sends right away is lost. */
    ESP_RETURN_ON_FALSE(xTaskCreate(slave_task, "qn_slave", 4096, NULL, SLAVE_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "slave task");
    ESP_RETURN_ON_FALSE(xTaskCreate(trace_task, "qn_trace", 3072, NULL, TRACE_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "trace task");

    const i2c_slave_config_t sc = {
        .i2c_port = cfg->i2c_port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 64,
        .receive_buf_depth = 64,
        .slave_addr = cfg->address,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        /* The head unit's bus already has pull-ups. */
    };
    ESP_RETURN_ON_ERROR(i2c_new_slave_device(&sc, &s.dev), TAG, "slave device");
    const i2c_slave_event_callbacks_t cbs = {.on_request = on_request, .on_receive = on_receive};
    ESP_RETURN_ON_ERROR(i2c_slave_register_event_callbacks(s.dev, &cbs, NULL), TAG, "callbacks");

    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.stats.running = true;
    xSemaphoreGive(s.lock);
    ESP_LOGI(TAG, "emulating QN8035 at 0x%02x on I2C%d (SDA %d, SCL %d, INT %d)%s", cfg->address,
             cfg->i2c_port, cfg->sda_gpio, cfg->scl_gpio, cfg->int_gpio,
             cfg->trace ? ", tracing" : "");
    return ESP_OK;
}

void qn8035_slave_complete_tune(uint32_t khz, bool found)
{
    if (!s.stats.running) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool irq = qn8035_emu_complete_tune(&s.emu, khz, found);
    xSemaphoreGive(s.lock);
    if (irq) {
        pulse_int();
    }
}

void qn8035_slave_set_signal(uint8_t rssi, uint8_t snr, bool stereo)
{
    if (!s.stats.running) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    qn8035_emu_set_signal(&s.emu, rssi, snr, stereo);
    xSemaphoreGive(s.lock);
}

void qn8035_slave_push_rds(const uint16_t blocks[4], uint8_t err_mask, bool version_b, bool sync)
{
    if (!s.stats.running) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool irq = qn8035_emu_push_rds(&s.emu, blocks, err_mask, version_b, sync);
    xSemaphoreGive(s.lock);
    if (irq) {
        pulse_int();
    }
}

void qn8035_slave_get_stats(qn8035_slave_stats_t *out)
{
    if (s.lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.stats;
    out->dropped = s.isr_dropped;
    xSemaphoreGive(s.lock);
}
