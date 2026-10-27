idf_component_register(
    SRCS "web_ui.c" "web_ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer app_update esp_app_format bootloader_support
                  radio radio_mem log_stream qn8035_slave sysmon tuning tuning_store af_store
                  esp_hw_support
    EMBED_TXTFILES "www/index.html"
)
# httpd's only reaction to a failed select(); web_ui.c adds a back-off (ESP-IDF 5.4.0).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=httpd_sess_delete_invalid")
