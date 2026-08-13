idf_component_register(
    SRCS "mqtt_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt config_manager ha_discovery ota_manager somfy_rts esp_system
)
