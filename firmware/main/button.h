#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef void (*button_cb_t)(void);

// Sets up the button (GPIO 32) and ring LED (GPIO 33).
// on_press fires on release for a short press; on_long_press fires after a 2 s hold.
// Both run from the button task. Either may be NULL.
esp_err_t button_init(button_cb_t on_press, button_cb_t on_long_press);

// Turns the button's LED ring on or off.
void button_set_ring(bool on);