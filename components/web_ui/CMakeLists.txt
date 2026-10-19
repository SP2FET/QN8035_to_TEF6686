idf_component_register(
    SRCS "web_ui.c" "web_ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer app_update esp_app_format bootloader_support
                  radio log_stream qn8035_slave
    EMBED_TXTFILES "www/index.html"
)
