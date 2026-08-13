#pragma once
#include <stdlib.h>
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) do{ if(getenv("VERBOSE")) printf("[I %s] " fmt "\n", tag, ##__VA_ARGS__); }while(0)
#define ESP_LOGW(tag, fmt, ...) do{ printf("[W %s] " fmt "\n", tag, ##__VA_ARGS__); }while(0)
#define ESP_LOGE(tag, fmt, ...) do{ printf("[E %s] " fmt "\n", tag, ##__VA_ARGS__); }while(0)
#define ESP_LOGD(tag, fmt, ...) do{}while(0)
