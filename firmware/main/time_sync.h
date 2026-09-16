#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

// Sets the local timezone and starts SNTP. Call once after Wi-Fi is initialized.
esp_err_t time_sync_start(void);

// True once the clock has been set from the internet.
bool time_is_valid(void);

// Formats a time as ISO 8601 UTC, e.g. 2026-09-16T22:50:47Z (Shopify's format).
void time_format_utc(time_t t, char *out, size_t len);

// Formats a time in local time for logs, e.g. 2026-09-16 18:50:47 EDT.
void time_format_local(time_t t, char *out, size_t len);

// Returns the moment local midnight happened today.
time_t time_start_of_local_day(time_t now);