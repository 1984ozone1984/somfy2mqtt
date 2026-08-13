#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef union {
    struct {
        uint16_t duration0 : 15;
        uint16_t level0    : 1;
        uint16_t duration1 : 15;
        uint16_t level1    : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;
typedef struct rmt_channel_t *rmt_channel_handle_t;
typedef struct rmt_encoder_t *rmt_encoder_handle_t;
typedef enum { RMT_CLK_SRC_DEFAULT = 0 } rmt_clock_source_t;
typedef struct {
    int gpio_num; rmt_clock_source_t clk_src; uint32_t resolution_hz;
    size_t mem_block_symbols; int trans_queue_depth;
} rmt_tx_channel_config_t;
typedef struct { int loop_count; struct { int eot_level; } flags; } rmt_transmit_config_t;
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *c, rmt_channel_handle_t *h);
esp_err_t rmt_enable(rmt_channel_handle_t h);
esp_err_t rmt_transmit(rmt_channel_handle_t ch, rmt_encoder_handle_t e,
                       const void *payload, size_t size, const rmt_transmit_config_t *cfg);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t ch, int timeout_ms);
