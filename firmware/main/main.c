#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "wifi.h"
#include "time_sync.h"
#include "shopify.h"

#define POLL_INTERVAL_S        60     // normal time between checks
#define BACKOFF_START_S        10     // first retry delay after a failure
#define BACKOFF_MAX_S          600    // never wait longer than 10 minutes
#define TOKEN_REFRESH_MARGIN_S 3600   // refresh the token 1 hour before it expires

static const char *TAG = "main";

static char s_token[128];
static int64_t s_token_refresh_at_us = 0;  // 0 means "no token yet"
static char s_last_seen[32];               // newest order timestamp already announced
static shopify_today_t s_today;

static void print_today(const shopify_today_t *t)
{
    char now_str[40];
    time_format_local(time(NULL), now_str, sizeof(now_str));

    long long dollars = t->revenue_cents / 100;
    long long cents = t->revenue_cents % 100;
    if (cents < 0) {
        cents = -cents;
    }

    ESP_LOGI(TAG, "[%s] Today: %d orders | %lld.%02lld %s | %d units | %d shipped%s",
             now_str, t->orders, dollars, cents, t->currency, t->units, t->shipped,
             t->truncated ? " (over 50 orders, stats incomplete)" : "");
    ESP_LOGI(TAG, "Free heap: %u bytes (lowest ever: %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

// One check: make sure we're online, have the time and a token, then fetch today's orders.
static esp_err_t poll_once(void)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "Wi-Fi is down");
        if (wifi_reconnect(15000) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    if (!time_is_valid()) {
        ESP_LOGW(TAG, "Clock not synced yet, can't work out 'today'");
        return ESP_ERR_INVALID_STATE;
    }

    // esp_timer counts time since boot and never jumps, unlike the wall clock.
    int64_t now_us = esp_timer_get_time();
    if (s_token_refresh_at_us == 0 || now_us >= s_token_refresh_at_us) {
        int expires_in = 0;
        esp_err_t err = shopify_get_token(s_token, sizeof(s_token), &expires_in);
        if (err != ESP_OK) {
            return err;
        }
        int refresh_in = (expires_in > 2 * TOKEN_REFRESH_MARGIN_S)
                             ? expires_in - TOKEN_REFRESH_MARGIN_S
                             : expires_in / 2;
        s_token_refresh_at_us = now_us + (int64_t)refresh_in * 1000000LL;
        ESP_LOGI(TAG, "Got access token, refreshing in %d s", refresh_in);
    }

    time_t now = time(NULL);
    if (s_last_seen[0] == '\0') {
        // First poll after boot: orders that already exist aren't "new".
        time_format_utc(now, s_last_seen, sizeof(s_last_seen));
    }

    char since[32];
    time_format_utc(time_start_of_local_day(now), since, sizeof(since));

    esp_err_t err = shopify_fetch_today(s_token, since, s_last_seen, &s_today);
    if (err == SHOPIFY_ERR_AUTH) {
        s_token_refresh_at_us = 0;  // force a new token on the next attempt
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }

    if (s_today.new_orders > 0) {
        // This is where the chime and lights will go.
        ESP_LOGW(TAG, "*** %d NEW ORDER%s! Latest: %s ***",
                 s_today.new_orders, s_today.new_orders > 1 ? "S" : "", s_today.newest_name);
    }
    if (s_today.newest_created[0] && strcmp(s_today.newest_created, s_last_seen) > 0) {
        strlcpy(s_last_seen, s_today.newest_created, sizeof(s_last_seen));
    }

    print_today(&s_today);
    return ESP_OK;
}

static void notifier_task(void *arg)
{
    if (wifi_init_and_connect() != ESP_OK) {
        ESP_LOGW(TAG, "Initial Wi-Fi connection failed, will keep retrying");
    }
    time_sync_start();

    int backoff_s = BACKOFF_START_S;
    for (;;) {
        int wait_s;
        if (poll_once() == ESP_OK) {
            backoff_s = BACKOFF_START_S;
            wait_s = POLL_INTERVAL_S;
        } else {
            wait_s = backoff_s;
            ESP_LOGW(TAG, "Poll failed, retrying in %d s", wait_s);
            backoff_s = (backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff_s * 2;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }
}

void app_main(void)
{
    // Wi-Fi stores calibration data in NVS, so NVS must be initialized first.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    xTaskCreate(notifier_task, "notifier", 8192, NULL, 5, NULL);
}