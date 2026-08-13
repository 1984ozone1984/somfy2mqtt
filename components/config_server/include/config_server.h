#pragma once
#include "esp_err.h"

/** Start the always-on HTTP portal on port 80 (both STA and AP mode). */
esp_err_t config_server_start(void);
