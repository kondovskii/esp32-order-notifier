#pragma once
#include "esp_err.h"

// Device credentials, read from the NVS partition at boot.
// They are provisioned once from a CSV on the laptop, never compiled into the firmware.
typedef struct {
    char wifi_ssid[33];       // Wi-Fi allows up to 32 characters
    char wifi_pass[65];       // Wi-Fi allows up to 64 characters
    char shop[64];            // e.g. "isotropicbusiness"
    char client_id[64];
    char client_secret[128];
} creds_t;

// Loads every credential from NVS. Fails if any are missing.
esp_err_t creds_load(void);

// Returns the loaded credentials. Only valid after creds_load() succeeds.
const creds_t *creds_get(void);