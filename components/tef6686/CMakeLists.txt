idf_component_register(
    SRCS
        "tef6686.c"
        "patch/tef6686_patch_v102_p224.c"
        "patch/tef6686_patch_v205_p512.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "patch"
    REQUIRES esp_driver_i2c
)
