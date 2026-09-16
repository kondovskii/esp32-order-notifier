#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Starts Wi-Fi and waits for the first connection (or failure after retries).
esp_err_t wifi_init_and_connect(void);

// True while the ESP32 has an IP address.
bool wifi_is_connected(void);

// Tries to reconnect after the automatic retries gave up. Waits up to timeout_ms.
esp_err_t wifi_reconnect(int timeout_ms);