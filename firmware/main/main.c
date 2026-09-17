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
#include "oled.h"
#include "audio.h"

#define POLL_INTERVAL_S        60     // normal time between checks
#define BACKOFF_START_S        10     // first retry delay after a failure
#define BACKOFF_MAX_S          600    // never wait longer than 10 minutes
#define TOKEN_REFRESH_MARGIN_S 3600   // refresh the token 1 hour before it expires

static const char *TAG = "main";

static char s_token[128];
static int64_t s_token_refresh_at_us = 0;  // 0 means "no token yet"
static char s_last_seen[32];               // newest order timestamp already announced
static shopify_today_t s_today;
static bool s_have_data = false;           // true after the first successful poll
static bool s_oled_ok = false;

// ---------- Screens ----------

// Full-screen message, used before the first successful poll.
static void ui_status(const char *line1, const char *line2)
{
    if (!s_oled_ok) {
        return;
    }
    oled_clear();
    oled_text_centered(3, "ORDER NOTIFIER", 1, true);
    oled_hline(0, 13, OLED_WIDTH);
    oled_text_centered(28, line1, 1, true);
    if (line2) {
        oled_text_centered(40, line2, 1, true);
    }
    oled_flush();
}

static void ui_today(const shopify_today_t *t, bool online)
{
    if (!s_oled_ok) {
        return;
    }
    char buf[48];
    oled_clear();

    // Header: "TODAY" on the left, last update time on the right.
    oled_text(0, 0, "TODAY", 1, true);
    if (!online) {
        oled_text(40, 0, "OFFLINE", 1, true);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    oled_text(OLED_WIDTH - oled_text_width(buf, 1), 0, buf, 1, true);

    // Revenue, big and centered.
    long long cents = t->revenue_cents;
    snprintf(buf, sizeof(buf), "$%lld.%02lld", cents / 100, (cents < 0 ? -cents : cents) % 100);
    oled_text_centered(13, buf, 2, true);

    snprintf(buf, sizeof(buf), "%d orders  %d units", t->orders, t->units);
    oled_text(0, 32, buf, 1, true);
    snprintf(buf, sizeof(buf), "%d shipped", t->shipped);
    oled_text(0, 43, buf, 1, true);

    // Bottom bar: highlighted when there's a new order.
    if (t->new_orders > 0) {
        oled_fill_rect(0, 54, OLED_WIDTH, 10, true);
        snprintf(buf, sizeof(buf), "NEW ORDER %s", t->newest_name);
        oled_text_centered(55, buf, 1, false);
    } else if (t->newest_name[0]) {
        snprintf(buf, sizeof(buf), "latest %s", t->newest_name);
        oled_text(0, 55, buf, 1, true);
    }

    oled_flush();
}

// ---------- Polling ----------

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
        audio_chime();  // lights will hook in here too
        ESP_LOGW(TAG, "*** %d NEW ORDER%s! Latest: %s ***",
                 s_today.new_orders, s_today.new_orders > 1 ? "S" : "", s_today.newest_name);
    }
    if (s_today.newest_created[0] && strcmp(s_today.newest_created, s_last_seen) > 0) {
        strlcpy(s_last_seen, s_today.newest_created, sizeof(s_last_seen));
    }

    long long cents = s_today.revenue_cents;
    ESP_LOGI(TAG, "Today: %d orders | %lld.%02lld %s | %d units | %d shipped | heap %u (min %u)",
             s_today.orders, cents / 100, (cents < 0 ? -cents : cents) % 100, s_today.currency,
             s_today.units, s_today.shipped,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    return ESP_OK;
}

static void notifier_task(void *arg)
{
    ui_status("connecting to wifi...", NULL);
    if (wifi_init_and_connect() != ESP_OK) {
        ESP_LOGW(TAG, "Initial Wi-Fi connection failed, will keep retrying");
    }

    ui_status("syncing clock...", NULL);
    time_sync_start();

    ui_status("checking orders...", NULL);

    int backoff_s = BACKOFF_START_S;
    for (;;) {
        int wait_s;
        if (poll_once() == ESP_OK) {
            s_have_data = true;
            ui_today(&s_today, true);
            backoff_s = BACKOFF_START_S;
            wait_s = POLL_INTERVAL_S;
        } else {
            wait_s = backoff_s;
            ESP_LOGW(TAG, "Poll failed, retrying in %d s", wait_s);
            if (s_have_data) {
                ui_today(&s_today, false);  // keep showing the last stats, marked offline
            } else {
                ui_status("can't reach shopify", "retrying...");
            }
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

    // A broken display shouldn't stop the notifier from working.
    s_oled_ok = (oled_init() == ESP_OK);
    if (!s_oled_ok) {
        ESP_LOGE(TAG, "OLED init failed, continuing without display");
    }

    // Same idea for audio: no speaker is annoying, not fatal.
    if (audio_init() == ESP_OK) {
        audio_chime();  // boot test so you know the speaker works
    } else {
        ESP_LOGE(TAG, "Audio init failed, continuing without sound");
    }

    xTaskCreate(notifier_task, "notifier", 8192, NULL, 5, NULL);
}