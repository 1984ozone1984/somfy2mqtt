idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES system_core config_manager wifi_manager config_server
             mqtt_manager ha_discovery ota_manager somfy_rts
)
