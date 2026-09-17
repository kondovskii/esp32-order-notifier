#include <math.h>
#include <string.h>
#include "leds.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "esp_log.h"

#define PIN_DATA   27
#define NUM_LEDS   8

// Hard current cap. Every colour channel is scaled from 0-255 down to 0-MAX_LEVEL,
// so no animation can ever exceed it. At 48, all 8 LEDs full white draw roughly 90 mA,
// safely within USB power and the 1N4148's rating.
#define MAX_LEVEL  48

#define FRAME_MS    20           // 50 frames per second
#define RMT_RES_HZ  10000000     // RMT timing resolution: 1 tick = 0.1 microseconds
#define CELEBRATE_S 2.6f         // length of the new-order animation
#define PI_F        3.14159265f

static const char *TAG = "leds";
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static TaskHandle_t s_task;
static volatile leds_state_t s_state = LEDS_CONNECTING;

// WS2812B expects colours in green-red-blue order, 3 bytes per LED.
static uint8_t s_grb[NUM_LEDS * 3];

static void set_pixel(int i, uint8_t r, uint8_t g, uint8_t b)
{
    s_grb[i * 3]     = (uint16_t)g * MAX_LEVEL / 255;
    s_grb[i * 3 + 1] = (uint16_t)r * MAX_LEVEL / 255;
    s_grb[i * 3 + 2] = (uint16_t)b * MAX_LEVEL / 255;
}

static void fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < NUM_LEDS; i++) {
        set_pixel(i, r, g, b);
    }
}

// Sends the 24 bytes to the strip. The 20 ms gap before the next frame
// acts as the "reset" signal that tells the LEDs to latch the new colours.
static void show(void)
{
    rmt_transmit_config_t cfg = {.loop_count = 0};
    rmt_transmit(s_chan, s_enc, s_grb, sizeof(s_grb), &cfg);
}


// New-order animation: green fills left to right, pulses, then fades out.
static void render_celebration(float t)
{
    if (t < 0.64f) {
        int lit = (int)(t / 0.08f) + 1;  // one more LED every 80 ms
        for (int i = 0; i < NUM_LEDS; i++) {
            if (i < lit) {
                set_pixel(i, 0, 255, 60);
            } else {
                set_pixel(i, 0, 0, 0);
            }
        }
        return;
    }

    float p = t - 0.64f;
    float pulse = 0.55f + 0.45f * cosf(p * 2.0f * PI_F * 2.0f);  // 2 pulses per second
    float fade = 1.0f;
    if (p > 1.4f) {
        fade = 1.0f - (p - 1.4f) / (CELEBRATE_S - 0.64f - 1.4f);
        if (fade < 0.0f) {
            fade = 0.0f;
        }
    }
    fill(0, (uint8_t)(255 * pulse * fade), (uint8_t)(60 * pulse * fade));
}

static void render_state(void)
{
    switch (s_state) {
    case LEDS_CONNECTING: {
        // Breathing: brightness follows a slow wave, one breath every 2.5 s.
        float phase = (float)((esp_timer_get_time() / 1000) % 2500) / 2500.0f;
        float level = 0.5f - 0.5f * cosf(phase * 2.0f * PI_F);
        fill(0, 0, (uint8_t)(255 * level));
        break;
    }
    case LEDS_OFFLINE:
        fill(0, 0, 0);
        set_pixel(0, 120, 0, 0);
        break;
    case LEDS_OK:
    default:
        fill(0, 0, 0);
        break;
    }
}

static void leds_task(void *arg)
{
    int64_t celebrate_start = -1;
    for (;;) {
        // Doubles as the frame timer: waits 20 ms, or wakes early on leds_new_order().
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FRAME_MS)) > 0) {
            celebrate_start = esp_timer_get_time();
        }

        if (celebrate_start >= 0) {
            float t = (float)(esp_timer_get_time() - celebrate_start) / 1000000.0f;
            if (t < CELEBRATE_S) {
                render_celebration(t);
                show();
                continue;
            }
            celebrate_start = -1;
        }

        render_state();
        show();
    }
}

esp_err_t leds_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = PIN_DATA,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,    // the ESP32's native RMT block size per channel
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT channel failed: %s", esp_err_to_name(err));
        return err;
    }

    // Each data bit is one high pulse followed by one low pulse.
    // The length of the high part tells the LED whether it's a 0 or a 1.
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},  // 0.3 us high, 0.9 us low
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},  // 0.9 us high, 0.3 us low
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_enc);
    if (err == ESP_OK) {
        err = rmt_enable(s_chan);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT setup failed: %s", esp_err_to_name(err));
        return err;
    }

    fill(0, 0, 0);
    show();
    xTaskCreate(leds_task, "leds", 3072, NULL, 4, &s_task);
    ESP_LOGI(TAG, "%d LEDs ready (brightness cap %d/255)", NUM_LEDS, MAX_LEVEL);
    return ESP_OK;
}

void leds_set_state(leds_state_t state)
{
    s_state = state;
}

void leds_new_order(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}