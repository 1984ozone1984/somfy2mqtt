#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Somfy RTS button codes (upper nibble of frame byte 1) */
#define SOMFY_BTN_STOP  0x1
#define SOMFY_BTN_UP    0x2
#define SOMFY_BTN_DOWN  0x4
#define SOMFY_BTN_PROG  0x8

/**
 * Claim an RMT TX channel on tx_gpio and prepare the frame buffer.
 * remote_addr is the 24-bit emulated remote address.
 */
esp_err_t somfy_rts_init(uint8_t tx_gpio, uint32_t remote_addr);

/**
 * Build and transmit one command: the wake-up frame followed by two repeats,
 * exactly as the Arduino sketch did. Blocks until the last frame has left the
 * RMT peripheral (~350 ms). Increments and persists the rolling code first.
 */
esp_err_t somfy_rts_send(uint8_t button);

/** Rolling code that will be used by the *next* transmission. */
uint32_t somfy_rts_get_rolling_code(void);
