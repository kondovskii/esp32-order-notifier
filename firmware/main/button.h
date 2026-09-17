#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef void (*button_cb_t)(void);

// Sets up the button (GPIO 32) and ring LED (GPIO 33).
// on_press runs once per debounced press, from the button task.
esp_err_t button_init(button_cb_t on_press);

// Turns the button's LED ring on or off.
void button_set_ring(bool on);