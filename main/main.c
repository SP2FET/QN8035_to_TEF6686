/*
 * QN8035 -> TEF6686 bridge: application wiring.
 *
 * Owns the start-up order and turns Kconfig into component configuration.
 * Components never read CONFIG_BRIDGE_* themselves.
 */
#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_stream.h"
#include "net_wifi.h"
#include "nvs_flash.h"
#include "qn_bridge.h"
#include "radio.h"
#include "radio_mem.h"
#include "sdkconfig.h"
#include "tef6686.h"
#include "tuning_store.h"
#include "web_ui.h"

static const char *TAG = "main";

static esp_err_t init_power(void)
{
#if CONFIG_BRIDGE_CPU_DFS
    /* Keep APB at 80 MHz for I2C and native USB. The RTOS raises the CPU to
     * the configured maximum while tasks run. No sleep: USB must stay live. */
    const esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    ESP_RETURN_ON_ERROR(esp_pm_configure(&cfg), TAG, "power config");
    ESP_LOGI(TAG, "CPU idle 80 MHz, active %d MHz; light sleep disabled", cfg.max_freq_mhz);
#endif
    return ESP_OK;
}

#define TEF_I2C_PORT I2C_NUM_0 /* the head-unit side uses I2C_NUM_1 */
#define QN_I2C_PORT  I2C_NUM_1
#define PROBE_TIMEOUT_MS 50
/* A web OTA image must run this long before it is confirmed; a crash or reset earlier
 * makes the bootloader return to the previous image (BOOTLOADER_APP_ROLLBACK_ENABLE). */
#define OTA_PROBATION_S 30

#if CONFIG_BRIDGE_TEF_REFCLK_4M
#define TEF_REFCLK_HZ 4000000
#elif CONFIG_BRIDGE_TEF_REFCLK_9M216
#define TEF_REFCLK_HZ 9216000
#elif CONFIG_BRIDGE_TEF_REFCLK_55M
#define TEF_REFCLK_HZ 55466670
#else
#define TEF_REFCLK_HZ 12000000
#endif

#if CONFIG_BRIDGE_FM_DEEMPHASIS_75
#define FM_DEEMPHASIS 750
#else
#define FM_DEEMPHASIS 500
#endif

#if CONFIG_BRIDGE_QN_TRACE
#define QN_TRACE true
#else
#define QN_TRACE false
#endif

#if CONFIG_BRIDGE_TEF_PATCH_V102
#define TEF_PATCH TEF6686_PATCH_V102
#elif CONFIG_BRIDGE_TEF_PATCH_V205
#define TEF_PATCH TEF6686_PATCH_V205
#else
#define TEF_PATCH TEF6686_PATCH_AUTO
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t new_bus(int sda, int scl, i2c_master_bus_handle_t *bus)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = TEF_I2C_PORT,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* ~45 kOhm only: fit 2.2-4.7 kOhm to 3V3 if the module has none. */
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, bus);
}

/* Create the tuner bus; with AUTOSWAP, also try SDA/SCL exchanged. The bus
 * is returned even if nothing answers: the radio task keeps retrying. */
static esp_err_t init_tef_bus(i2c_master_bus_handle_t *bus)
{
    int sda = CONFIG_BRIDGE_TEF_I2C_SDA_GPIO;
    int scl = CONFIG_BRIDGE_TEF_I2C_SCL_GPIO;

    ESP_RETURN_ON_ERROR(new_bus(sda, scl, bus), TAG, "i2c bus");
    esp_err_t err = i2c_master_probe(*bus, TEF6686_I2C_ADDR_DEFAULT, PROBE_TIMEOUT_MS);
#if CONFIG_BRIDGE_TEF_I2C_AUTOSWAP
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no tuner on SDA=%d SCL=%d (%s), trying swapped pins", sda, scl,
                 esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(i2c_del_master_bus(*bus), TAG, "del bus");
        ESP_RETURN_ON_ERROR(new_bus(scl, sda, bus), TAG, "i2c bus (swapped)");
        err = i2c_master_probe(*bus, TEF6686_I2C_ADDR_DEFAULT, PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "tuner found with SDA=%d SCL=%d: fix the pins in menuconfig", scl, sda);
            return ESP_OK;
        }
        ESP_RETURN_ON_ERROR(i2c_del_master_bus(*bus), TAG, "del bus");
        ESP_RETURN_ON_ERROR(new_bus(sda, scl, bus), TAG, "i2c bus");
    }
#endif
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "tuner ACKs 0x%02x on SDA=%d SCL=%d", TEF6686_I2C_ADDR_DEFAULT, sda, scl);
    } else {
        ESP_LOGE(TAG, "no ACK from 0x%02x: check power, wiring and pull-ups; will keep retrying",
                 TEF6686_I2C_ADDR_DEFAULT);
    }
    return ESP_OK;
}

/* Blocks the main task (which has nothing else to do) for the probation time. */
static void confirm_ota_image(void)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "new firmware on probation: confirming in %d s, a reset before that rolls back",
             OTA_PROBATION_S);
    vTaskDelay(pdMS_TO_TICKS(OTA_PROBATION_S * 1000));
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "new firmware confirmed");
    } else {
        ESP_LOGE(TAG, "confirming firmware failed (%s): the next reset rolls back",
                 esp_err_to_name(err));
    }
}

void app_main(void)
{
    /* First, so the web console also shows the boot messages. */
    ESP_ERROR_CHECK(log_stream_init(CONFIG_BRIDGE_LOG_RING_SIZE));

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (ESP-IDF %s), reset reason %d", app->project_name, app->version,
             app->idf_ver, (int)esp_reset_reason());

    ESP_ERROR_CHECK(init_power());
    ESP_ERROR_CHECK(init_nvs());

    i2c_master_bus_handle_t tef_bus;
    ESP_ERROR_CHECK(init_tef_bus(&tef_bus));

    radio_config_t radio_cfg = {
        .tef = {
            .bus = tef_bus,
            .address = TEF6686_I2C_ADDR_DEFAULT,
            .scl_speed_hz = CONFIG_BRIDGE_TEF_I2C_FREQ_HZ,
            .ref_clock_hz = TEF_REFCLK_HZ,
            .ref_clock_type = TEF_REFCLK_HZ == 55466670 ? TEF6686_REFCLK_EXTERNAL
                                                         : TEF6686_REFCLK_CRYSTAL,
            .patch = TEF_PATCH,
        },
        .fm_min_khz = CONFIG_BRIDGE_FM_MIN_KHZ,
        .fm_max_khz = CONFIG_BRIDGE_FM_MAX_KHZ,
        .fm_step_khz = CONFIG_BRIDGE_FM_STEP_KHZ,
        .am_step_khz = CONFIG_BRIDGE_AM_STEP_KHZ,
        .initial = {
            .band = RADIO_BAND_FM,
            .khz = CONFIG_BRIDGE_FM_DEFAULT_KHZ,
            /* -60 dB would be one step below the TEF's -59.9 dB */
            .volume = CONFIG_BRIDGE_DEFAULT_VOLUME_DB < -59 ? -599
                                                            : CONFIG_BRIDGE_DEFAULT_VOLUME_DB * 10,
            .bw_auto = true,
        },
        .fm_deemphasis = FM_DEEMPHASIS,
    };
    /* Reception tuning (DSP, AF, seek) is set in the web UI and saved there; not fatal: the
     * defaults then apply and a save reports the problem. */
    if (tuning_store_init(&radio_cfg.tuning) != ESP_OK) {
        ESP_LOGE(TAG, "saved reception tuning not loaded, using the defaults");
    }

#if CONFIG_BRIDGE_QN_EMU_ENABLE
    ESP_ERROR_CHECK(qn_bridge_init());
#endif
#if CONFIG_BRIDGE_MEMORY
    /* Not fatal: without it the radio starts from the defaults and the web UI hides memory. */
    const bool memory = radio_mem_init(&radio_cfg.initial) == ESP_OK;
#endif
    ESP_ERROR_CHECK(radio_start(&radio_cfg));
#if CONFIG_BRIDGE_MEMORY
    if (memory && radio_mem_start() != ESP_OK) {
        ESP_LOGE(TAG, "settings autosave not running");
    }
#endif

#if CONFIG_BRIDGE_QN_EMU_ENABLE
    /* Before Wi-Fi: the head unit may probe the chip only once at its boot. */
    const qn8035_slave_config_t qn_cfg = {
        .i2c_port = QN_I2C_PORT,
        .sda_gpio = CONFIG_BRIDGE_QN_I2C_SDA_GPIO,
        .scl_gpio = CONFIG_BRIDGE_QN_I2C_SCL_GPIO,
        .int_gpio = CONFIG_BRIDGE_QN_INT_GPIO,
        .address = QN8035_I2C_ADDR,
        .trace = QN_TRACE,
    };
    ESP_ERROR_CHECK(qn_bridge_start(&qn_cfg));
#else
    ESP_LOGI(TAG, "QN8035 emulation disabled (menuconfig: QN8035 emulation)");
#endif

#if CONFIG_BRIDGE_WIFI_ENABLE
    const net_wifi_config_t wifi_cfg = {
        .ap_ssid = CONFIG_BRIDGE_WIFI_AP_SSID,
        .ap_password = CONFIG_BRIDGE_WIFI_AP_PASSWORD,
        .ap_channel = CONFIG_BRIDGE_WIFI_AP_CHANNEL,
        .sta_ssid = CONFIG_BRIDGE_WIFI_STA_SSID,
        .sta_password = CONFIG_BRIDGE_WIFI_STA_PASSWORD,
    };
    ESP_ERROR_CHECK(net_wifi_start(&wifi_cfg));
    const web_ui_config_t web_cfg = {
        .ota_password = CONFIG_BRIDGE_OTA_PASSWORD,
    };
    ESP_ERROR_CHECK(web_ui_start(&web_cfg));
#else
    ESP_LOGI(TAG, "Wi-Fi and web UI disabled; use USB to update firmware");
#endif

    confirm_ota_image();
}
