idf_component_register(
    SRCS "qn_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES qn8035_slave
    PRIV_REQUIRES radio
)
