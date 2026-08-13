/*
 * Host test: does the RMT symbol stream produced by somfy_rts.c reproduce the
 * exact waveform the Arduino sketch bit-banged?
 *
 * The real somfy_rts.c is compiled in (against stub ESP-IDF headers), so this
 * tests the shipping code, not a copy of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "config_manager.h"
#include "somfy_rts.h"

/* ── Stub state ───────────────────────────────────────────────────────────── */

somfy_config_t g_config = { .rolling_code = 42, .remote_addr = 0x121309, .tx_gpio = 15 };

esp_err_t config_manager_save_rolling_code(uint32_t code)
{
    g_config.rolling_code = code;
    return ESP_OK;
}

static rmt_symbol_word_t captured[512];
static size_t captured_n;

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *c, rmt_channel_handle_t *h)
{ (void)c; *h = (rmt_channel_handle_t)1; return ESP_OK; }
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *c, rmt_encoder_handle_t *h)
{ (void)c; *h = (rmt_encoder_handle_t)1; return ESP_OK; }
esp_err_t rmt_enable(rmt_channel_handle_t h) { (void)h; return ESP_OK; }
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t ch, int t) { (void)ch; (void)t; return ESP_OK; }

esp_err_t rmt_transmit(rmt_channel_handle_t ch, rmt_encoder_handle_t e,
                       const void *payload, size_t size, const rmt_transmit_config_t *cfg)
{
    (void)ch; (void)e; (void)cfg;
    captured_n = size / sizeof(rmt_symbol_word_t);
    memcpy(captured, payload, size);
    return ESP_OK;
}

/* ── Pulse lists ──────────────────────────────────────────────────────────── */

typedef struct { uint8_t level; uint32_t us; } pulse_t;

typedef struct { pulse_t p[4096]; size_t n; } plist_t;

static void push(plist_t *l, uint8_t level, uint32_t us)
{
    if (us == 0) return;
    l->p[l->n].level = level;
    l->p[l->n].us    = us;
    l->n++;
}

/* Merge adjacent same-level runs so a split duration compares equal to a
 * single long one, and so "LOW 640 then LOW 640" compares equal to "LOW 1280". */
static void normalize(const plist_t *in, plist_t *out)
{
    out->n = 0;
    for (size_t i = 0; i < in->n; i++) {
        if (out->n > 0 && out->p[out->n - 1].level == in->p[i].level) {
            out->p[out->n - 1].us += in->p[i].us;
        } else {
            out->p[out->n] = in->p[i];
            out->n++;
        }
    }
}

/* ── Reference implementation ──────────────────────────────────────────────────
 * Transcribed verbatim from the Arduino sketch this firmware replaces
 * (BuildFrame/SendCommand in Nickduino's Somfy_Remote). This is the frozen
 * definition of "correct" — the waveform is known to drive real hardware, so it
 * is reproduced here rather than referenced, and must not be "cleaned up".
 * -------------------------------------------------------------------------- */

#define SYMBOL 640
#define REMOTE 0x121309

static void ref_BuildFrame(uint8_t *frame, uint8_t button, unsigned int code)
{
    frame[0] = 0xA7;
    frame[1] = button << 4;
    frame[2] = code >> 8;
    frame[3] = code;
    frame[4] = (uint8_t)(REMOTE >> 16);
    frame[5] = (uint8_t)(REMOTE >> 8);
    frame[6] = (uint8_t)REMOTE;

    uint8_t checksum = 0;
    for (uint8_t i = 0; i < 7; i++) checksum = checksum ^ frame[i] ^ (frame[i] >> 4);
    checksum &= 0b1111;
    frame[1] |= checksum;

    for (uint8_t i = 1; i < 7; i++) frame[i] ^= frame[i - 1];
}

static void ref_SendCommand(plist_t *l, const uint8_t *frame, uint8_t sync)
{
    if (sync == 2) {
        push(l, 1, 9415);
        push(l, 0, 89565);
    }
    for (int i = 0; i < sync; i++) {
        push(l, 1, 4 * SYMBOL);
        push(l, 0, 4 * SYMBOL);
    }
    push(l, 1, 4550);
    push(l, 0, SYMBOL);

    for (uint8_t i = 0; i < 56; i++) {
        if (((frame[i / 8] >> (7 - (i % 8))) & 1) == 1) {
            push(l, 0, SYMBOL);
            push(l, 1, SYMBOL);
        } else {
            push(l, 1, SYMBOL);
            push(l, 0, SYMBOL);
        }
    }
    push(l, 0, 30415);
}

/* ── Checks ───────────────────────────────────────────────────────────────── */

static int failures;

static void check(bool cond, const char *what)
{
    printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

static void expand_captured(plist_t *l)
{
    l->n = 0;
    for (size_t i = 0; i < captured_n; i++) {
        push(l, captured[i].level0, captured[i].duration0);
        push(l, captured[i].level1, captured[i].duration1);
    }
}

static void test_button(uint8_t button, const char *name)
{
    printf("\n== %s (0x%X) ==\n", name, button);

    uint32_t code_before = g_config.rolling_code;

    captured_n = 0;
    esp_err_t err = somfy_rts_send(button);
    check(err == ESP_OK, "somfy_rts_send returned ESP_OK");
    check(captured_n > 0 && captured_n <= 208, "symbol count within SYM_CAP");
    printf("     (%zu symbols, rolling code %u -> %u)\n",
           captured_n, code_before, g_config.rolling_code);
    check(g_config.rolling_code == code_before + 1, "rolling code advanced by 1");

    /* Reference waveform for the same code */
    uint8_t frame[7];
    ref_BuildFrame(frame, button, (unsigned int)code_before);

    plist_t ref = {0}, refn = {0};
    ref_SendCommand(&ref, frame, 2);
    ref_SendCommand(&ref, frame, 7);
    ref_SendCommand(&ref, frame, 7);
    normalize(&ref, &refn);

    plist_t got = {0}, gotn = {0};
    expand_captured(&got);
    normalize(&got, &gotn);

    check(refn.n == gotn.n, "same number of level transitions");

    size_t n = refn.n < gotn.n ? refn.n : gotn.n;
    size_t mismatches = 0;
    for (size_t i = 0; i < n; i++) {
        if (refn.p[i].level != gotn.p[i].level || refn.p[i].us != gotn.p[i].us) {
            if (mismatches < 5) {
                printf("       [%zu] arduino=%s %uus   rmt=%s %uus\n", i,
                       refn.p[i].level ? "HIGH" : "LOW ", refn.p[i].us,
                       gotn.p[i].level ? "HIGH" : "LOW ", gotn.p[i].us);
            }
            mismatches++;
        }
    }
    check(mismatches == 0, "every pulse matches the Arduino waveform");

    /* No RMT duration field may exceed 15 bits, and only the final one may be
     * the zero end-marker. */
    bool durations_ok = true;
    for (size_t i = 0; i < captured_n; i++) {
        if (captured[i].duration0 == 0) durations_ok = false;
        if (captured[i].duration1 == 0 && i != captured_n - 1) durations_ok = false;
    }
    check(durations_ok, "no premature zero-duration end marker");

    /* Total airtime sanity */
    uint64_t total = 0;
    for (size_t i = 0; i < gotn.n; i++) total += gotn.p[i].us;
    printf("     (total airtime %.1f ms)\n", total / 1000.0);
    /* 216.5 ms wake frame + 2 x 143.1 ms repeats = 502.7 ms */
    check(total > 495000 && total < 510000, "airtime in the expected ~503 ms range");
}

static void test_frame_bytes(void)
{
    printf("\n== frame integrity ==\n");
    /* Deobfuscate what the reference built and confirm the nibble XOR is zero,
     * which is the property the blind actually checks. */
    uint8_t frame[7];
    ref_BuildFrame(frame, SOMFY_BTN_UP, 1234);
    for (int i = 6; i >= 1; i--) frame[i] ^= frame[i - 1];

    uint8_t x = 0;
    for (int i = 0; i < 7; i++) x ^= frame[i] ^ (frame[i] >> 4);
    check((x & 0x0F) == 0, "nibble checksum over the deobfuscated frame is zero");
    check(frame[0] == 0xA7, "byte 0 is the 0xA7 key");
    check((frame[1] >> 4) == SOMFY_BTN_UP, "button nibble preserved");
    check(((frame[2] << 8) | frame[3]) == 1234, "rolling code preserved");
    check(((frame[4] << 16) | (frame[5] << 8) | frame[6]) == 0x121309,
          "remote address preserved");
}

int main(void)
{
    printf("Somfy RTS waveform equivalence test\n");

    esp_err_t err = somfy_rts_init(15, 0x121309);
    if (err != ESP_OK) { printf("init failed\n"); return 1; }

    test_button(SOMFY_BTN_UP,   "UP");
    test_button(SOMFY_BTN_DOWN, "DOWN");
    test_button(SOMFY_BTN_STOP, "STOP");
    test_button(SOMFY_BTN_PROG, "PROG");
    test_frame_bytes();

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
