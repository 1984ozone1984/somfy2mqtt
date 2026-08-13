idf_component_register(
    SRCS "somfy_rts.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt config_manager
)
