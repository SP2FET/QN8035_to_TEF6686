#include "sysmon.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "qn8035_slave.h"
#include "sdkconfig.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

static const char *TAG = "sysmon";

#define TASK_STACK         4096
#define TASK_PRIO          1
#define SNAPSHOT_PERIOD_MS 500
#define SLOT_MAGIC         0x51544546u /* "QTEF" */
#define CRASH_MAGIC        0x43524153u /* "CRAS" */
#define NVS_NAMESPACE      "sysmon"
#define NVS_KEY_HIST       "resets"
#define CLOCK_SET_AFTER    1600000000  /* UTC seconds: below this the clock was not set */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t elf[8]; /* start of the app's ELF SHA-256: this build only */
    sysmon_state_t st;
    uint32_t crc;   /* of everything before */
} slot_t;

/* The NVS blob: all-time count and the last resets, newest first. */
typedef struct {
    uint32_t total;
    uint8_t n;
    uint8_t reserved[3];
    sysmon_reset_t e[SYSMON_HIST_MAX];
} hist_t;

/* RTC slow memory keeps these over every reset but a power cycle. Two slots, written in turn: a
 * reset in the middle of a write leaves the other one intact. */
static RTC_NOINIT_ATTR slot_t s_slots[2];
static RTC_NOINIT_ATTR uint32_t s_crash_magic;
static RTC_NOINIT_ATTR uint32_t s_crashes;

static struct {
    SemaphoreHandle_t lock; /* guards info */
    sysmon_info_t info;
    bool have_warm;
    sysmon_state_t warm;
    uint32_t seq;           /* of the last slot written; sysmon task after init */
    sysmon_state_t last;    /* sysmon task only */
} s;

const char *sysmon_reset_name(int reason)
{
    switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "reset pin";
    case ESP_RST_SW:         return "software restart";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep sleep";
    case ESP_RST_BROWNOUT:   return "brown-out";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lock-up";
    default:                 return "unknown";
    }
}

static bool unexpected(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT || r == ESP_RST_PWR_GLITCH ||
           r == ESP_RST_CPU_LOCKUP;
}

static uint32_t slot_crc(const slot_t *sl)
{
    return esp_rom_crc32_le(0, (const uint8_t *)sl, offsetof(slot_t, crc));
}

static const uint8_t *build_id(void)
{
    return esp_app_get_description()->app_elf_sha256;
}

static bool slot_ok(const slot_t *sl)
{
    return sl->magic == SLOT_MAGIC && memcmp(sl->elf, build_id(), sizeof(sl->elf)) == 0 &&
           sl->crc == slot_crc(sl);
}

/* The reset history from NVS; an empty one if there is none or it does not fit. */
static void hist_load(hist_t *h)
{
    memset(h, 0, sizeof(*h));
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nh) != ESP_OK) {
        return;
    }
    size_t len = sizeof(*h);
    if (nvs_get_blob(nh, NVS_KEY_HIST, h, &len) != ESP_OK || len != sizeof(*h) ||
        h->n > SYSMON_HIST_MAX) {
        memset(h, 0, sizeof(*h));
    }
    nvs_close(nh);
}

/* Called before the QN8035 slave starts: the flash write cannot delay a head-unit read. */
static void hist_add(hist_t *h, const sysmon_reset_t *r)
{
    memmove(&h->e[1], &h->e[0], (SYSMON_HIST_MAX - 1) * sizeof(h->e[0]));
    h->e[0] = *r;
    h->n = (uint8_t)MIN(h->n + 1u, (unsigned)SYSMON_HIST_MAX);
    h->total++;
    nvs_handle_t nh;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nh);
    if (err == ESP_OK) {
        err = nvs_set_blob(nh, NVS_KEY_HIST, h, sizeof(*h));
        if (err == ESP_OK) {
            err = nvs_commit(nh);
        }
        nvs_close(nh);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset history not saved: %s", esp_err_to_name(err));
    }
}

esp_err_t sysmon_init(void)
{
    ESP_RETURN_ON_FALSE(s.lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    s.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_NO_MEM, TAG, "mutex");

    const esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_POWERON || s_crash_magic != CRASH_MAGIC) {
        s_crash_magic = CRASH_MAGIC;
        s_crashes = 0;
    }
    if (unexpected(r)) {
        s_crashes++;
    }
    s.info.reset = sysmon_reset_name(r);
    s.info.unexpected = unexpected(r);
    s.info.crashes = s_crashes;

    const slot_t *best = NULL;
    for (unsigned i = 0; r != ESP_RST_POWERON && i < 2; i++) {
        if (slot_ok(&s_slots[i]) && (best == NULL || (int32_t)(s_slots[i].seq - best->seq) > 0)) {
            best = &s_slots[i];
        }
    }
    if (best) {
        s.warm = best->st;
        s.have_warm = true;
        s.seq = best->seq;
        s.last = s.warm;
    }
    s.info.warm = s.have_warm;

    hist_t h;
    hist_load(&h);
    if (s.info.unexpected && s_crashes <= SYSMON_HIST_WRITES) {
        /* A snapshot of another build still tells how long the previous run lasted. */
        const slot_t *prev = NULL;
        for (unsigned i = 0; i < 2; i++) {
            const slot_t *sl = &s_slots[i];
            if (sl->magic == SLOT_MAGIC && sl->crc == slot_crc(sl) &&
                (prev == NULL || (int32_t)(sl->seq - prev->seq) > 0)) {
                prev = sl;
            }
        }
        const sysmon_reset_t rec = {
            .reason = (uint8_t)r,
            .count = (uint8_t)MIN(s_crashes, (uint32_t)UINT8_MAX),
            .up_s = prev ? prev->st.up_s : 0,
            .wall_s = prev ? prev->st.wall_s : 0,
        };
        hist_add(&h, &rec);
    }
    s.info.resets = h.total;
    s.info.hist_n = h.n;
    memcpy(s.info.hist, h.e, sizeof(s.info.hist));

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t addr, size;
    s.info.dump = esp_core_dump_image_get(&addr, &size) == ESP_OK;
#endif

    if (s.info.unexpected) {
        ESP_LOGW(TAG, "reset by %s, %lu since power-on, %lu remembered", s.info.reset,
                 (unsigned long)s_crashes, (unsigned long)h.total);
    } else {
        ESP_LOGI(TAG, "reset: %s", s.info.reset);
    }
    if (s.have_warm && s.warm.radio_valid) {
        ESP_LOGI(TAG, "resuming %s %lu kHz%s", s.warm.radio.band == RADIO_BAND_FM ? "FM" : "AM",
                 (unsigned long)s.warm.radio.khz,
                 s.warm.qn_valid ? " and the head unit's QN8035 state" : "");
    }
    return ESP_OK;
}

const sysmon_state_t *sysmon_warm_state(void)
{
    return s.have_warm ? &s.warm : NULL;
}

static void read_dump_summary(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (!s.info.dump) {
        return;
    }
    static esp_core_dump_summary_t sum; /* sysmon task only; keeps it off the stack */
    const esp_err_t err = esp_core_dump_get_summary(&sum);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "core dump stored, no summary: %s", esp_err_to_name(err));
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    sysmon_info_t *in = &s.info;
    memcpy(in->task, sum.exc_task, sizeof(in->task) - 1);
    in->task[sizeof(in->task) - 1] = '\0';
    memcpy(in->elf, sum.app_elf_sha256, MIN(sizeof(in->elf) - 1, sizeof(sum.app_elf_sha256)));
    in->elf[sizeof(in->elf) - 1] = '\0';
    in->pc = sum.exc_pc;
    in->bt_n = (uint8_t)MIN(sum.exc_bt_info.depth, (uint32_t)SYSMON_BT_MAX);
    in->bt_corrupted = sum.exc_bt_info.corrupted;
    memcpy(in->bt, sum.exc_bt_info.bt, in->bt_n * sizeof(in->bt[0]));
    const sysmon_info_t info = *in;
    xSemaphoreGive(s.lock);

    char bt[SYSMON_BT_MAX * 11 + 1] = "";
    for (unsigned i = 0, pos = 0; i < info.bt_n; i++) {
        pos += (unsigned)snprintf(&bt[pos], sizeof(bt) - pos, " 0x%08lx",
                                  (unsigned long)info.bt[i]);
    }
    ESP_LOGW(TAG, "core dump of build %s: task %s, PC 0x%08lx, backtrace%s%s (GET /api/coredump)",
             info.elf, info.task, (unsigned long)info.pc, bt,
             info.bt_corrupted ? " (corrupted)" : "");
#endif
}

static void snapshot(void)
{
    sysmon_state_t *st = &s.last;
    radio_settings_t rs;
    qn8035_emu_t before, after;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        const bool qn = qn8035_slave_snapshot(&before);
        const bool settled = radio_get_restart_settings(&rs);
        const bool qn_after = qn8035_slave_snapshot(&after);
        if (qn != qn_after || (qn &&
            (before.revision != after.revision || before.op != after.op ||
             before.op_generation != after.op_generation || before.tuned_ch != after.tuned_ch))) {
            continue;
        }
        if (!settled || (qn && after.op != QN8035_OP_NONE)) {
            continue; /* retain the entire previous pair during a transition */
        }
        st->radio_valid = true;
        st->radio = rs;
        st->qn_valid = qn;
        if (qn) {
            st->qn = after;
        }
        break;
    }
    st->up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    const time_t now = time(NULL);
    st->wall_s = now > CLOCK_SET_AFTER ? (int64_t)now : 0;

    slot_t n;
    memset(&n, 0, sizeof(n));
    n.magic = SLOT_MAGIC;
    n.seq = ++s.seq;
    memcpy(n.elf, build_id(), sizeof(n.elf));
    n.st = *st;
    n.crc = slot_crc(&n);
    s_slots[n.seq & 1u] = n;
}

static void sysmon_task(void *arg)
{
    (void)arg;
    read_dump_summary();
    for (;;) {
        snapshot();
        vTaskDelay(pdMS_TO_TICKS(SNAPSHOT_PERIOD_MS));
    }
}

esp_err_t sysmon_start(void)
{
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    const BaseType_t ok = xTaskCreate(sysmon_task, "sysmon", TASK_STACK, NULL, TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void sysmon_get_info(sysmon_info_t *out)
{
    if (s.lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->reset = "unknown";
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.info;
    xSemaphoreGive(s.lock);
}

esp_err_t sysmon_dump_size(size_t *size)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t addr;
    return esp_core_dump_image_get(&addr, size) == ESP_OK ? ESP_OK : ESP_ERR_NOT_FOUND;
#else
    (void)size;
    return ESP_ERR_NOT_FOUND;
#endif
}

esp_err_t sysmon_dump_read(size_t offset, void *buf, size_t len)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t addr, size;
    ESP_RETURN_ON_FALSE(esp_core_dump_image_get(&addr, &size) == ESP_OK, ESP_ERR_NOT_FOUND, TAG,
                        "no core dump");
    ESP_RETURN_ON_FALSE(offset <= size && len <= size - offset, ESP_ERR_INVALID_SIZE, TAG,
                        "beyond the core dump");
    return esp_flash_read(esp_flash_default_chip, buf, (uint32_t)(addr + offset), (uint32_t)len);
#else
    (void)offset;
    (void)buf;
    (void)len;
    return ESP_ERR_NOT_FOUND;
#endif
}

esp_err_t sysmon_dump_erase(uint32_t crc32)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t addr, size;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t buf[256];
    uint32_t crc = 0; /* the ROM function inverts on entry and exit: chaining equals zlib.crc32 */
    for (size_t off = 0; off < size;) {
        const size_t n = MIN(sizeof(buf), size - off);
        ESP_RETURN_ON_ERROR(
            esp_flash_read(esp_flash_default_chip, buf, (uint32_t)(addr + off), (uint32_t)n), TAG,
            "read");
        crc = esp_rom_crc32_le(crc, buf, (uint32_t)n);
        off += n;
    }
    if (crc != crc32) {
        ESP_LOGW(TAG, "core dump kept: CRC %08lx, caller's %08lx", (unsigned long)crc,
                 (unsigned long)crc32);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_RETURN_ON_ERROR(esp_core_dump_image_erase(), TAG, "erase");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.info.dump = false;
    s.info.task[0] = '\0';
    s.info.elf[0] = '\0';
    s.info.pc = 0;
    s.info.bt_n = 0;
    s.info.bt_corrupted = false;
    memset(s.info.bt, 0, sizeof(s.info.bt));
    xSemaphoreGive(s.lock);
    ESP_LOGI(TAG, "core dump erased (CRC %08lx)", (unsigned long)crc);
    return ESP_OK;
#else
    (void)crc32;
    return ESP_ERR_NOT_FOUND;
#endif
}
