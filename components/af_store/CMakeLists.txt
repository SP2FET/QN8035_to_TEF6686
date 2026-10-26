idf_component_register(
    SRCS "af_store.c"
    INCLUDE_DIRS "include"
    REQUIRES af
    PRIV_REQUIRES radio nvs_flash
)
