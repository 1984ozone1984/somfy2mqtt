idf_component_register(
    SRCS "config_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server config_manager wifi_manager somfy_rts
             mqtt_manager ha_discovery esp_system app_update
)
