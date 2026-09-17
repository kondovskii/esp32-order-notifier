#pragma once
#include "esp_err.h"

typedef enum {
    LEDS_CONNECTING,  // slow blue "breathing" while starting up
    LEDS_OK,          // off: quiet desk when everything's working
    LEDS_OFFLINE,     // first LED dim red when Shopify can't be reached
} leds_state_t;

// Sets up the RMT peripheral and starts the animation task.
esp_err_t leds_init(void);

// Changes the background state shown between celebrations.
void leds_set_state(leds_state_t state);

// Plays the new-order animation once. Returns immediately.
void leds_new_order(void);