idf_component_register(
    SRCS "tuning_store.c"
    INCLUDE_DIRS "include"
    REQUIRES tuning
    PRIV_REQUIRES nvs_flash
)
