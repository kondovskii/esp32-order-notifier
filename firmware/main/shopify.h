#pragma once
#include <stddef.h>
#include "esp_err.h"

// Exchanges the client ID + secret for an access token (valid ~24 h).
esp_err_t shopify_get_token(char *token_out, size_t token_len, int *expires_in);

// Fetches the 5 most recent orders and prints them to the serial monitor.
esp_err_t shopify_print_recent_orders(const char *token);