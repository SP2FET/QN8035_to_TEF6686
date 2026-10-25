idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_i2c
        esp_app_format
        esp_pm
        app_update
        nvs_flash
        tef6686
        tuning_store
        radio
        radio_mem
        bridge
        qn8035_slave
        log_stream
        net_wifi
        web_ui
        sysmon
)
