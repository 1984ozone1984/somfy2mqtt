#pragma once
#include "driver/rmt_tx.h"
typedef struct { } rmt_copy_encoder_config_t;
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *c, rmt_encoder_handle_t *h);
