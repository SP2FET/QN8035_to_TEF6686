#include <assert.h>
#include <setjmp.h>
#define MOCK_TASK_DELAY_HOOK
#include "components/af_store/af_store.c"

TickType_t mock_tick;
static uint8_t version;
static bool have_version, present[AF_MEM_SLOTS];
static af_mem_slot_t records[AF_MEM_SLOTS];
static af_mem_t radio_memory;
static unsigned writes, periods, stop_after;
static jmp_buf stop;

void mock_task_delay(TickType_t t)
{
    if (periods++ == stop_after) {
        longjmp(stop, 1);
    }
    mock_tick += t;
}

esp_err_t nvs_open(const char *n, int mode, nvs_handle_t *h) { *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) {}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{
    *v = version;
    return have_version ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{
    version = v; have_version = true; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len)
{
    const unsigned i = (unsigned)atoi(k + 1);
    if (!present[i]) { return ESP_ERR_NVS_NOT_FOUND; }
    assert(*len >= sizeof(records[i]));
    memcpy(v, &records[i], sizeof(records[i])); *len = sizeof(records[i]); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    const unsigned i = (unsigned)atoi(k + 1);
    assert(len == sizeof(records[i]));
    memcpy(&records[i], v, len); present[i] = true; writes++; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    const unsigned i = (unsigned)atoi(k + 1);
    if (!present[i]) { return ESP_ERR_NVS_NOT_FOUND; }
    present[i] = false; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }
void radio_get_af_mem(af_mem_t *out) { *out = radio_memory; }

static void reboot(void)
{
    free(s.w);
    memset(&s, 0, sizeof(s));
}

int main(void)
{
    version = AF_MEM_VERSION + 1; have_version = true; present[0] = true;
    records[0] = (af_mem_slot_t){.pi = 0x1234, .code = 70};
    af_mem_t loaded;
    assert(af_store_init(&loaded) == ESP_OK);
    assert(af_mem_count(&loaded) == 0 && !present[0] && version == AF_MEM_VERSION);
    reboot();
    assert(af_store_init(&loaded) == ESP_OK && af_mem_count(&loaded) == 0);
    puts("PASS incompatible AF records remain absent on the second boot");

    const uint8_t codes[] = {70, 88};
    af_mem_learn(&radio_memory, 0x1234, 92000, codes, 2);
    const af_store_config_t cfg = {0};
    assert(af_store_start(&cfg) == ESP_OK && s.running);
    stop_after = 35;
    if (setjmp(stop) == 0) { store_task(NULL); }
    assert(writes == 1 && present[0] && records[0].n == 2);
    puts("PASS automatic AF flash writes remain enabled after settle/quiet deadlines");
    reboot();
    assert(af_store_init(&loaded) == ESP_OK && af_mem_count(&loaded) == 1);
    assert(loaded.s[0].pi == 0x1234 && loaded.s[0].n == 2);
    puts("PASS automatically stored AF list reloads after reboot");
    reboot();
    return 0;
}
