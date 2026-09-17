#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define PIN_BUTTON 32   // switch to GND; internal pull-up holds it high when released
#define PIN_RING   33   // LED ring, through a 1N4148

#define SAMPLE_MS        10  // check the pin 100 times per second
#define DEBOUNCE_SAMPLES 3   // a change must hold for 3 samples (30 ms) to count

static const char *TAG = "button";
static button_cb_t s_on_press;

// Debouncing by sampling: the pin has to read the new level several times in a row
// before we believe it changed. Contact bounce flips back within a few ms, which
// resets the count, so it never gets through.
static void button_task(void *arg)
{
    int stable = 1;       // last confirmed level: 1 = released, 0 = pressed
    int count = 0;        // consecutive samples that disagree with 'stable'
    int prev_raw = 1;
    int raw_changes = 0;  // every flip seen on the pin, bounces included (for the log)

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        int raw = gpio_get_level(PIN_BUTTON);

        if (raw != prev_raw) {
            raw_changes++;
            prev_raw = raw;
        }

        if (raw == stable) {
            count = 0;  // matches what we already believe; any bounce is over
            continue;
        }

        if (++count >= DEBOUNCE_SAMPLES) {
            stable = raw;
            count = 0;
            if (stable == 0) {
                ESP_LOGI(TAG, "Press (%d raw pin changes since last press)", raw_changes);
                raw_changes = 0;
                if (s_on_press) {
                    s_on_press();
                }
            }
        }
    }
}

esp_err_t button_init(button_cb_t on_press)
{
    s_on_press = on_press;

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&in);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t out = {
        .pin_bit_mask = 1ULL << PIN_RING,
        .mode = GPIO_MODE_OUTPUT,
    };
    err = gpio_config(&out);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PIN_RING, 0);

    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Button ready on GPIO %d", PIN_BUTTON);
    return ESP_OK;
}

void button_set_ring(bool on)
{
    gpio_set_level(PIN_RING, on ? 1 : 0);
}