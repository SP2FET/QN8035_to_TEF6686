idf_component_register(
    SRCS "radio.c"
    INCLUDE_DIRS "include"
    REQUIRES tef6686 rds af tuning
    PRIV_REQUIRES trace
)
