#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Returned when Shopify rejects the access token (HTTP 401), so the caller can refresh it.
#define SHOPIFY_ERR_AUTH 0x6001

typedef struct {
    int orders;                 // today's orders, excluding cancelled
    int units;                  // items sold today
    int shipped;                // today's orders already fulfilled
    int64_t revenue_cents;      // money as whole cents, avoiding float rounding
    char currency[8];
    char newest_name[16];       // e.g. "#2454" ("" if no orders today)
    char newest_created[32];    // ISO UTC timestamp of the newest order
    int new_orders;             // orders created after last_seen
    bool truncated;             // more than one page of orders today
} shopify_today_t;

// Exchanges the client ID + secret for an access token (valid ~24 h).
esp_err_t shopify_get_token(char *token_out, size_t token_len, int *expires_in);

// Fetches orders created since since_utc (start of today, ISO UTC) and fills in stats.
// Orders created after last_seen (ISO UTC) are counted as new.
esp_err_t shopify_fetch_today(const char *token, const char *since_utc,
                              const char *last_seen, shopify_today_t *out);