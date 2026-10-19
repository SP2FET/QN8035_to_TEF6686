idf_component_register(
    SRCS "radio_mem.c"
    INCLUDE_DIRS "include"
    REQUIRES radio presets
    PRIV_REQUIRES nvs_flash
)
