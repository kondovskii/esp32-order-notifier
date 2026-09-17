#include <math.h>
#include "audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

// Wiring to the MAX98357A.
#define PIN_BCLK 26   // bit clock
#define PIN_LRC  25   // left/right (word select) clock
#define PIN_DIN  22   // audio data

#define SAMPLE_RATE 22050   // samples per second; plenty for simple tones
#define VOLUME      0.15f   // fraction of full scale. Raise slowly: too loud can brown out USB power
#define CHUNK       256     // samples generated per write

#define TWO_PI 6.2831853f

static const char *TAG = "audio";
static i2s_chan_handle_t s_tx;
static TaskHandle_t s_task;

// Generates one note as a sine wave and streams it to the amplifier.
// decay: how quickly the note fades, like a struck bell (bigger = shorter).
static void play_note(float freq_hz, int duration_ms, float decay)
{
    int16_t buf[CHUNK];
    int total = SAMPLE_RATE * duration_ms / 1000;
    int ramp = SAMPLE_RATE * 5 / 1000;            // 5 ms fade in/out avoids clicks
    float phase = 0.0f;
    float step = TWO_PI * freq_hz / SAMPLE_RATE;  // how far along the wave each sample moves

    int n = 0;
    while (n < total) {
        int count = (total - n < CHUNK) ? (total - n) : CHUNK;
        for (int i = 0; i < count; i++, n++) {
            float t = (float)n / SAMPLE_RATE;
            float env = expf(-decay * t);         // natural fade-out
            if (n < ramp) {
                env *= (float)n / ramp;           // fade in
            }
            int left = total - n;
            if (left < ramp) {
                env *= (float)left / ramp;        // fade out at the very end
            }
            buf[i] = (int16_t)(32767.0f * VOLUME * env * sinf(phase));
            phase += step;
            if (phase >= TWO_PI) {
                phase -= TWO_PI;
            }
        }
        size_t written = 0;
        i2s_channel_write(s_tx, buf, count * sizeof(int16_t), &written, pdMS_TO_TICKS(1000));
    }
}

// Waits for a chime request, plays it, repeats.
static void audio_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // sleep until audio_chime() is called
        play_note(880.0f, 160, 8.0f);    // A5
        play_note(1318.5f, 500, 5.0f);   // E6, a rising "ding-DING"
    }
}

esp_err_t audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // send silence when there's nothing to play
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // the MAX98357A doesn't need a master clock
            .bclk = PIN_BCLK,
            .ws = PIN_LRC,
            .dout = PIN_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S setup failed: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(audio_task, "audio", 4096, NULL, 6, &s_task);
    ESP_LOGI(TAG, "I2S audio ready at %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

void audio_chime(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}