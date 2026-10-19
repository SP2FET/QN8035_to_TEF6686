idf_component_register(
    SRCS "web_ui.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer radio log_stream qn8035_slave
    EMBED_TXTFILES "www/index.html"
)
