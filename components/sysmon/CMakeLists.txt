idf_component_register(
    SRCS "sysmon.c"
    INCLUDE_DIRS "include"
    REQUIRES radio qn8035_emu
    PRIV_REQUIRES qn8035_slave espcoredump spi_flash esp_app_format esp_rom esp_timer nvs_flash
)
