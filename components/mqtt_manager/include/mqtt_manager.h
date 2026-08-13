#pragma once
#include <stdbool.h>

void mqtt_manager_init(void);
int  mqtt_publish(const char *topic, const char *payload, int qos, int retain);
bool mqtt_manager_is_connected(void);
