#include "qn8035_slave.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "driver/i2c_slave.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/i2c_ll.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if CONFIG_ESP_TASK_WDT_EN
#include "esp_task_wdt.h"
#endif

#if !CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2
#error "qn8035_slave needs CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2=y (see sdkconfig.defaults)"
#endif
#if !CONFIG_I2C_ISR_IRAM_SAFE
#error "QN8035 emulation with AF persistence requires CONFIG_I2C_ISR_IRAM_SAFE=y"
#endif

static const char *TAG = "qn8035";

#define MSG_QUEUE_LEN   16
#define TRACE_QUEUE_LEN 32
#define TRACE_BYTES     8
#define INT_PULSE_US    4500 /* datasheet: 4.5 ms low pulse */
#define SLAVE_TASK_PRIO (configMAX_PRIORITIES - 2)
#define TRACE_TASK_PRIO 2
#define TX_PRELOAD      SOC_I2C_FIFO_LEN
/* I2C input filter in APB cycles: 15 = 187.5 ns at 80 MHz, the S2 maximum. The v2 slave driver
 * never sets it (only the master driver does); the head unit's shortest SCL phase is 1.4 us. */
#define GLITCH_FILTER_CYCLES 15
/* The slave task waits at most this long, so the task watchdog can watch it. */
#define WDT_FEED_MS 1000

typedef enum {
    MSG_WRITE,
    MSG_READ,
} msg_type_t;

/* One bus write (with the reads the master did before it), or reads found by a refresh. */
typedef struct {
    uint8_t wlen;                  /* bytes written, data[0] = register; 0 = reads only */
    uint8_t wdata[TRACE_BYTES];
    uint8_t rlen;                  /* bytes the master read from the TX FIFO */
    uint8_t rreg;                  /* register of the first byte read */
    uint8_t rdata[TRACE_BYTES];
    uint8_t nevt;
    uint32_t revision;
    qn8035_evt_t evts[QN8035_EMU_MAX_EVENTS];
} bus_msg_t;

typedef struct {
    uint8_t type;
    uint8_t reg;
    uint8_t len; /* data bytes, may exceed TRACE_BYTES */
    uint8_t data[TRACE_BYTES];
} trace_rec_t;

static struct {
    portMUX_TYPE mux;        /* guards emu, tx and the TX FIFO: the ISR and the setters use them */
    qn8035_emu_t emu;
    i2c_dev_t *hw;
    uint8_t tx[TX_PRELOAD];  /* what the TX FIFO was filled with */
    uint8_t tx_n;
    uint8_t tx_reg;          /* register of tx[0] */

    SemaphoreHandle_t lock;  /* guards stats */
    QueueHandle_t msgs;
    QueueHandle_t traces;
    i2c_slave_dev_handle_t dev;
    gpio_num_t int_gpio;
    esp_timer_handle_t int_timer;
    qn8035_slave_event_cb_t cb;
    void *cb_ctx;
    bool trace;
    volatile uint32_t dropped;
    bool reconcile;
    uint32_t command_drops;
    qn8035_slave_stats_t stats;
} s = {.mux = portMUX_INITIALIZER_UNLOCKED};

/* ---- TX FIFO (caller holds s.mux) ------------------------------------------ */

/*
 * The head unit's master (QF30 MCU, docs/headunit-mcu-firmware.md) never reads SCL back, so the
 * slave must not stretch: the S2 would hold SCL low at the read address and the master would
 * clock garbage. With stretching off the hardware sends whatever is in the TX FIFO, so the FIFO
 * always holds the registers from the read pointer on. It is refilled when a write sets the
 * pointer (the STOP of "write register" comes ~60 us before the master clocks the first byte of
 * its read) and when the tuner side changes the model while the bus is idle.
 */

/* Bytes the master clocked out since the last fill: they were read, the pointer moves on. */
static void IRAM_ATTR take_reads_locked(bus_msg_t *m)
{
    uint32_t free_len = 0;
    i2c_ll_get_txfifo_len(s.hw, &free_len);
    const uint32_t left = SOC_I2C_FIFO_LEN - free_len;
    if (left >= s.tx_n) {
        return;
    }
    const uint8_t n = (uint8_t)(s.tx_n - left);
    m->rlen = n;
    m->rreg = s.tx_reg;
    memcpy(m->rdata, s.tx, MIN((size_t)n, sizeof(m->rdata)));
    qn8035_emu_skip(&s.emu, n);
    s.tx_n = 0;
}

static void IRAM_ATTR fill_tx_locked(void)
{
    i2c_ll_txfifo_rst(s.hw);
    s.tx_reg = s.emu.ptr;
    qn8035_emu_peek_block(&s.emu, s.tx, sizeof(s.tx));
    i2c_ll_write_txfifo(s.hw, s.tx, sizeof(s.tx));
    s.tx_n = sizeof(s.tx);
}

/* After a model change from the tuner side. Mid-transaction the FIFO is left alone: the
 * next register write refills it anyway. */
static void refresh_tx_locked(bus_msg_t *m)
{
    if (i2c_ll_is_bus_busy(s.hw)) {
        return;
    }
    take_reads_locked(m);
    fill_tx_locked();
}

/* ---- ISR side ------------------------------------------------------------- */

/* Runs at the STOP of a write (the v2 driver calls it only when bytes arrived). */
static bool IRAM_ATTR on_receive(i2c_slave_dev_handle_t dev, const i2c_slave_rx_done_event_data_t *evt,
                       void *arg)
{
    (void)dev;
    (void)arg;
    bus_msg_t m = {0};
    const size_t len = evt->length;

    portENTER_CRITICAL_ISR(&s.mux);
    take_reads_locked(&m);
    m.nevt = (uint8_t)qn8035_emu_write(&s.emu, evt->buffer, len, m.evts, QN8035_EMU_MAX_EVENTS);
    m.revision = s.emu.revision;
    fill_tx_locked();
    portEXIT_CRITICAL_ISR(&s.mux);

    m.wlen = (uint8_t)MIN(len, (size_t)UINT8_MAX);
    memcpy(m.wdata, evt->buffer, MIN(len, sizeof(m.wdata)));
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s.msgs, &m, &woken) != pdTRUE) {
        s.dropped++;
        if (m.nevt) {
            portENTER_CRITICAL_ISR(&s.mux);
            s.reconcile = true;
            s.command_drops++;
            portEXIT_CRITICAL_ISR(&s.mux);
        }
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
        s.dropped++; /* counted together; both mean lost visibility */
    }
}

static void post_reads(const bus_msg_t *m)
{
    if (m->rlen && xQueueSend(s.msgs, m, 0) != pdTRUE) {
        s.dropped++;
    }
}

static void slave_task(void *arg)
{
    (void)arg;
    bus_msg_t m;
    uint32_t reconciled = 0;
    bool have_reconciled = false;

#if CONFIG_ESP_TASK_WDT_EN
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog: not watched");
    }
#endif
    for (;;) {
#if CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif
        qn8035_emu_t model;
        portENTER_CRITICAL(&s.mux);
        const bool reconcile = s.reconcile;
        if (reconcile) {
            model = s.emu;
            s.reconcile = false;
        }
        portEXIT_CRITICAL(&s.mux);
        if (reconcile) {
            qn8035_evt_t evts[QN8035_EMU_MAX_EVENTS];
            const size_t n = qn8035_emu_controls(&model, evts, QN8035_EMU_MAX_EVENTS);
            reconciled = model.revision;
            have_reconciled = true;
            xSemaphoreTake(s.lock, portMAX_DELAY);
            s.stats.reconciles++;
            xSemaphoreGive(s.lock);
            for (size_t i = 0; i < n; i++) {
                s.cb(&evts[i], s.cb_ctx);
            }
        }
        if (xQueueReceive(s.msgs, &m, pdMS_TO_TICKS(WDT_FEED_MS)) != pdTRUE) {
            continue;
        }
        xSemaphoreTake(s.lock, portMAX_DELAY);
        s.stats.reads += m.rlen;
        s.stats.writes += m.wlen ? 1u : 0u;
        s.stats.cmds += m.wlen > 1u ? 1u : 0u;
        xSemaphoreGive(s.lock);
        /* Reads show up late (at the next write or refresh) but in bus order. */
        if (m.rlen) {
            trace(MSG_READ, m.rreg, m.rdata, m.rlen);
        }
        if (m.wlen) {
            trace(MSG_WRITE, m.wdata[0], &m.wdata[1], m.wlen - 1u);
        }
        const bool stale = have_reconciled && (int32_t)(m.revision - reconciled) <= 0;
        for (size_t i = 0; !stale && i < m.nevt; i++) {
            s.cb(&m.evts[i], s.cb_ctx);
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
    ESP_RETURN_ON_FALSE(cfg->i2c_port >= 0 && cfg->i2c_port < SOC_I2C_NUM, ESP_ERR_INVALID_ARG, TAG,
                        "i2c port");

    if (cfg->restore) {
        s.emu = *cfg->restore;
    } else {
        qn8035_emu_init(&s.emu);
    }
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
        /* The head unit's bus has its own pull-ups. The internal ones (~45 kOhm) add a little to
         * them against the capacitance of the leads to the ESP. */
        .flags.enable_internal_pullup = true,
    };
    s.hw = I2C_LL_GET_HW(cfg->i2c_port);
    ESP_RETURN_ON_ERROR(i2c_new_slave_device(&sc, &s.dev), TAG, "slave device");
    /* No on_request: it only comes from the stretch interrupt, which is switched off below.
     * i2c_slave_write() is never used either; the FIFO is ours. */
    const i2c_slave_event_callbacks_t cbs = {.on_receive = on_receive};
    ESP_RETURN_ON_ERROR(i2c_slave_register_event_callbacks(s.dev, &cbs, NULL), TAG, "callbacks");

    /* The v2 driver always enables stretching and has no option against it. */
    portENTER_CRITICAL(&s.mux);
    i2c_ll_slave_enable_scl_stretch(s.hw, false);
    i2c_ll_master_set_filter(s.hw, GLITCH_FILTER_CYCLES); /* the controller's filter, slave mode too */
    i2c_ll_update(s.hw);
    fill_tx_locked();
    portEXIT_CRITICAL(&s.mux);

    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.stats.running = true;
    xSemaphoreGive(s.lock);
    ESP_LOGI(TAG, "emulating QN8035 at 0x%02x on I2C%d (SDA %d, SCL %d, INT %d), "
             "no clock stretching, %d ns input filter%s%s",
             cfg->address, cfg->i2c_port, cfg->sda_gpio, cfg->scl_gpio, cfg->int_gpio,
             GLITCH_FILTER_CYCLES * 25 / 2, cfg->trace ? ", tracing" : "",
             cfg->restore ? ", registers restored" : "");
    return ESP_OK;
}

bool qn8035_slave_snapshot(qn8035_emu_t *out)
{
    if (!s.stats.running) {
        return false;
    }
    portENTER_CRITICAL(&s.mux);
    *out = s.emu;
    portEXIT_CRITICAL(&s.mux);
    return true;
}

void qn8035_slave_complete_tune(uint32_t khz, bool found, bool by_scan, uint32_t generation)
{
    if (!s.stats.running) {
        return;
    }
    bus_msg_t m = {0};
    portENTER_CRITICAL(&s.mux);
    const bool irq = generation == s.emu.op_generation &&
                     qn8035_emu_complete_tune(&s.emu, khz, found, by_scan);
    refresh_tx_locked(&m);
    portEXIT_CRITICAL(&s.mux);
    post_reads(&m);
    if (irq) {
        pulse_int();
    }
}

void qn8035_slave_set_signal(uint8_t rssi, uint8_t snr, bool stereo)
{
    if (!s.stats.running) {
        return;
    }
    bus_msg_t m = {0};
    portENTER_CRITICAL(&s.mux);
    qn8035_emu_set_signal(&s.emu, rssi, snr, stereo);
    refresh_tx_locked(&m);
    portEXIT_CRITICAL(&s.mux);
    post_reads(&m);
}

void qn8035_slave_push_rds(const uint16_t blocks[4], uint8_t err_mask, bool version_b, bool sync)
{
    if (!s.stats.running) {
        return;
    }
    bus_msg_t m = {0};
    portENTER_CRITICAL(&s.mux);
    const bool irq = qn8035_emu_push_rds(&s.emu, blocks, err_mask, version_b, sync);
    refresh_tx_locked(&m);
    portEXIT_CRITICAL(&s.mux);
    post_reads(&m);
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
    out->dropped = s.dropped;
    portENTER_CRITICAL(&s.mux);
    out->command_drops = s.command_drops;
    portEXIT_CRITICAL(&s.mux);
    xSemaphoreGive(s.lock);
}

void qn8035_slave_request_reconcile(void)
{
    if (!s.msgs) {
        return;
    }
    portENTER_CRITICAL(&s.mux);
    s.reconcile = true;
    portEXIT_CRITICAL(&s.mux);
    const bus_msg_t wake = {0};
    xQueueSend(s.msgs, &wake, 0); /* a full queue already wakes the consumer */
}

void qn8035_slave_set_rds_sync(bool sync)
{
    if (!s.stats.running) {
        return;
    }
    bus_msg_t m = {0};
    portENTER_CRITICAL(&s.mux);
    qn8035_emu_set_rds_sync(&s.emu, sync);
    refresh_tx_locked(&m);
    portEXIT_CRITICAL(&s.mux);
    post_reads(&m);
}
