idf_component_register(
    SRCS "web_ui.c" "web_ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer app_update esp_app_format bootloader_support
                  radio radio_mem log_stream qn8035_slave sysmon tuning tuning_store
    EMBED_TXTFILES "www/index.html"
)
