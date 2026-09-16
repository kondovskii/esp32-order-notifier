#pragma once
#include "esp_err.h"

// Connects to the network in secrets.h. Blocks until connected or failed.
esp_err_t wifi_connect(void);