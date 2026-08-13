/**
 * Somfy RTS transmitter.
 *
 * Frame layout, checksum and obfuscation are a straight port of the Arduino
 * sketch (originally Nickduino's). What changed is how the waveform reaches the
 * pin: the sketch bit-banged it with delayMicroseconds(), which only worked
 * because the Arduino loop had nothing else to do. Under ESP-IDF the WiFi stack
 * and lwIP run on the same core, and a 350 ms bit-bang would either be shredded
 * by their interrupts or would starve them if we masked interrupts to protect
 * it. So the whole command — wake-up frame plus two repeats — is rendered into
 * RMT symbols and clocked out by the RMT peripheral at 1 µs resolution. Timing
 * is then hardware-exact regardless of what the CPU is doing.
 *
 * Protocol reference: https://pushstack.wordpress.com/somfy-rts-protocol/
 */

#include "somfy_rts.h"
#include "config_manager.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "somfy_rts";

#define SYMBOL_US        640      /* half-bit period of the Manchester coding   */
#define WAKE_HIGH_US     9415
#define WAKE_LOW_US      89565
#define SW_SYNC_HIGH_US  4550
#define INTERFRAME_US    30415
#define FRAME_BITS       56

#define RMT_RESOLUTION_HZ  1000000  /* 1 tick = 1 µs                            */
#define RMT_MAX_TICKS      32767    /* duration field is 15 bits                */
#define RMT_MEM_SYMBOLS    128      /* 2 hardware blocks; refilled by ISR        */

/* One command = frame(sync=2) + 2×frame(sync=7) ≈ 384 level changes = 192
 * symbols. Rounded up so a future extra repeat cannot silently truncate. */
#define SYM_CAP            208

static rmt_channel_handle_t s_chan     = NULL;
static rmt_encoder_handle_t s_encoder  = NULL;
static SemaphoreHandle_t    s_lock     = NULL;
static uint32_t             s_remote   = 0;
static rmt_symbol_word_t    s_symbols[SYM_CAP];

/* ── Symbol emitter ───────────────────────────────────────────────────────────
 * Appends (level, duration) pairs, packing two per RMT symbol and splitting any
 * duration longer than the 15-bit field — the 89.6 ms wake-up silence needs it.
 * -------------------------------------------------------------------------- */

typedef struct {
    rmt_symbol_word_t *sym;
    size_t             cap;
    size_t             count;   /* completed symbols                           */
    bool               half;    /* next write goes into the second half         */
    bool               overflow;
} emitter_t;

static void emit(emitter_t *e, uint8_t level, uint32_t us)
{
    while (us > 0) {
        uint32_t chunk = (us > RMT_MAX_TICKS) ? RMT_MAX_TICKS : us;
        us -= chunk;

        if (!e->half) {
            if (e->count >= e->cap) { e->overflow = true; return; }
            e->sym[e->count].level0    = level;
            e->sym[e->count].duration0 = chunk;
            e->half = true;
        } else {
            e->sym[e->count].level1    = level;
            e->sym[e->count].duration1 = chunk;
            e->count++;
            e->half = false;
        }
    }
}

/* Close a dangling half-symbol. A zero duration is RMT's end marker, which is
 * exactly right here because this is the last symbol of the transmission. */
static void emit_finish(emitter_t *e)
{
    if (e->half && e->count < e->cap) {
        e->sym[e->count].level1    = 0;
        e->sym[e->count].duration1 = 0;
        e->count++;
        e->half = false;
    }
}

/* ── Frame construction ─────────────────────────────────────────────────────── */

static void build_frame(uint8_t *frame, uint8_t button, uint16_t code, uint32_t remote)
{
    frame[0] = 0xA7;                    /* encryption key — value is arbitrary   */
    frame[1] = button << 4;             /* low nibble becomes the checksum       */
    frame[2] = code >> 8;               /* rolling code, big endian              */
    frame[3] = code & 0xFF;
    frame[4] = (uint8_t)(remote >> 16);
    frame[5] = (uint8_t)(remote >> 8);
    frame[6] = (uint8_t)remote;

    /* Checksum: XOR of all nibbles, so that the full XOR comes out zero */
    uint8_t checksum = 0;
    for (int i = 0; i < 7; i++) {
        checksum ^= frame[i] ^ (frame[i] >> 4);
    }
    frame[1] |= checksum & 0x0F;

    ESP_LOGI(TAG, "frame  %02X %02X %02X %02X %02X %02X %02X (code=%u remote=0x%06lX)",
             frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6],
             code, (unsigned long)remote);

    /* Obfuscation: rolling XOR over the payload */
    for (int i = 1; i < 7; i++) {
        frame[i] ^= frame[i - 1];
    }
}

/* sync = 2 for the first frame (preceded by the wake-up pulse), 7 for repeats */
static void emit_frame(emitter_t *e, const uint8_t *frame, int sync)
{
    if (sync == 2) {
        emit(e, 1, WAKE_HIGH_US);
        emit(e, 0, WAKE_LOW_US);
    }

    for (int i = 0; i < sync; i++) {          /* hardware sync                   */
        emit(e, 1, 4 * SYMBOL_US);
        emit(e, 0, 4 * SYMBOL_US);
    }

    emit(e, 1, SW_SYNC_HIGH_US);              /* software sync                   */
    emit(e, 0, SYMBOL_US);

    for (int i = 0; i < FRAME_BITS; i++) {    /* MSB first, Manchester coded     */
        if ((frame[i / 8] >> (7 - (i % 8))) & 1) {
            emit(e, 0, SYMBOL_US);
            emit(e, 1, SYMBOL_US);
        } else {
            emit(e, 1, SYMBOL_US);
            emit(e, 0, SYMBOL_US);
        }
    }

    emit(e, 0, INTERFRAME_US);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

esp_err_t somfy_rts_init(uint8_t tx_gpio, uint32_t remote_addr)
{
    s_remote = remote_addr & 0xFFFFFF;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = tx_gpio,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .mem_block_symbols = RMT_MEM_SYMBOLS,
        .trans_queue_depth = 2,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_copy_encoder_config_t enc_cfg = {};   /* the struct has no fields */
    err = rmt_new_copy_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "init done — GPIO%u, remote 0x%06lX, next rolling code %lu",
             tx_gpio, (unsigned long)s_remote,
             (unsigned long)g_config.rolling_code);
    return ESP_OK;
}

esp_err_t somfy_rts_send(uint8_t button)
{
    if (!s_chan || !s_encoder) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    uint16_t code = (uint16_t)g_config.rolling_code;

    /* Persist the successor *before* transmitting. If power drops mid-frame the
     * blind may already have accepted this code, and replaying it would be
     * rejected — burning one code is always cheaper than desynchronising. */
    config_manager_save_rolling_code(g_config.rolling_code + 1);

    uint8_t frame[7];
    build_frame(frame, button, code, s_remote);

    emitter_t e = { .sym = s_symbols, .cap = SYM_CAP };
    emit_frame(&e, frame, 2);   /* wake-up frame */
    emit_frame(&e, frame, 7);   /* repeat        */
    emit_frame(&e, frame, 7);   /* repeat        */
    emit_finish(&e);

    esp_err_t err;
    if (e.overflow) {
        ESP_LOGE(TAG, "symbol buffer overflow — command not sent");
        err = ESP_ERR_NO_MEM;
    } else {
        rmt_transmit_config_t tx_cfg = {
            .loop_count  = 0,
            .flags.eot_level = 0,   /* leave the transmitter keyed off */
        };
        err = rmt_transmit(s_chan, s_encoder, s_symbols,
                           e.count * sizeof(rmt_symbol_word_t), &tx_cfg);
        if (err == ESP_OK) {
            err = rmt_tx_wait_all_done(s_chan, 2000);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transmit failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "sent button 0x%X in %u symbols", button, (unsigned)e.count);
        }
    }

    xSemaphoreGive(s_lock);
    return err;
}

uint32_t somfy_rts_get_rolling_code(void)
{
    return g_config.rolling_code;
}
