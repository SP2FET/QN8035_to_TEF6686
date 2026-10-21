idf_component_register(
    SRCS "qn8035_slave.c"
    INCLUDE_DIRS "include"
    REQUIRES qn8035_emu esp_driver_gpio
    PRIV_REQUIRES esp_driver_i2c esp_timer hal soc
)
