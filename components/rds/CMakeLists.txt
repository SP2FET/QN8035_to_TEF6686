# Pure C (no ESP-IDF APIs) so it can be unit-tested on the host: pio test -e native
idf_component_register(
    SRCS "rds_decoder.c"
    INCLUDE_DIRS "include"
)
