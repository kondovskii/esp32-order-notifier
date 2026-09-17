#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "wifi.h"
#include "time_sync.h"
#include "shopify.h"
#include "oled.h"
#include "audio.h"
#include "leds.h"
#include "button.h"
#include "creds.h"
#include "ota.h"

#define POLL_INTERVAL_S        60     // normal time between checks
#define BACKOFF_START_S        10     // first retry delay after a failure
#define BACKOFF_MAX_S          600    // never wait longer than 10 minutes
#define TOKEN_REFRESH_MARGIN_S 3600   // refresh the token 1 hour before it expires
#define PAGE_TIMEOUT_S         30     // return to the overview after this long without a press
#define SELFTEST_MAX_FAILS     3      // failed polls after an update before rolling back

typedef enum {
    PAGE_OVERVIEW,
    PAGE_REVENUE,
    PAGE_UNITS,
    PAGE_SHIPPED,
    PAGE_DEVICE,
    PAGE_COUNT,
} page_t;

static const char *TAG = "main";

// ---------- Network state (only used by the notifier task) ----------
static char s_token[128];
static int64_t s_token_refresh_at_us = 0;  // 0 means "no token yet"
static char s_last_seen[32];               // newest order timestamp already announced

// ---------- Display state ----------
// Two tasks touch this (notifier and button), so every read/write happens
// while holding s_ui_mutex. Otherwise one task could draw half a screen
// while the other changes the data underneath it.
static SemaphoreHandle_t s_ui_mutex;
static bool s_oled_ok = false;
static shopify_today_t s_today;
static bool s_have_data = false;
static bool s_online = false;
static bool s_unread = false;              // a new order hasn't been acknowledged yet
static page_t s_page = PAGE_OVERVIEW;
static int64_t s_last_press_us = 0;
static char s_status[32] = "starting...";
static bool s_ota_busy = false;
static bool s_pending_verify = false;   // running brand-new firmware that must prove itself
static int s_selftest_fails = 0;

// ---------- Drawing helpers (call with the mutex held) ----------

static void format_money(int64_t cents, char *out, size_t len)
{
    long long c = cents;
    snprintf(out, len, "$%lld.%02lld", c / 100, (c < 0 ? -c : c) % 100);
}

// Draws text as big as possible (up to max_scale) while still fitting the width.
static void draw_big_centered(int y, const char *s, int max_scale)
{
    int scale = max_scale;
    while (scale > 1 && oled_text_width(s, scale) > OLED_WIDTH) {
        scale--;
    }
    oled_text_centered(y, s, scale, true);
}

static void draw_header(const char *title)
{
    oled_text(0, 0, title, 1, true);
    if (!s_online) {
        oled_text(OLED_WIDTH - oled_text_width("OFFLINE", 1), 0, "OFFLINE", 1, true);
    } else {
        char buf[16];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        oled_text(OLED_WIDTH - oled_text_width(buf, 1), 0, buf, 1, true);
    }
    oled_hline(0, 9, OLED_WIDTH);
}

// Bottom row: the NEW ORDER bar if unread, otherwise dots showing which page this is.
static void draw_footer(void)
{
    if (s_unread) {
        char buf[32];
        oled_fill_rect(0, 54, OLED_WIDTH, 10, true);
        snprintf(buf, sizeof(buf), "NEW ORDER %s", s_today.newest_name);
        oled_text_centered(55, buf, 1, false);
        return;
    }

    const int dot = 4, gap = 4;
    int total = PAGE_COUNT * dot + (PAGE_COUNT - 1) * gap;
    int x = (OLED_WIDTH - total) / 2;
    for (int i = 0; i < PAGE_COUNT; i++) {
        oled_fill_rect(x, 58, dot, dot, true);
        if (i != s_page) {
            oled_fill_rect(x + 1, 59, dot - 2, dot - 2, false);  // hollow = not this page
        }
        x += dot + gap;
    }
}

// ---------- Pages ----------

static void page_overview(const shopify_today_t *t)
{
    char buf[48];
    draw_header("TODAY v2");

    format_money(t->revenue_cents, buf, sizeof(buf));
    draw_big_centered(13, buf, 2);

    snprintf(buf, sizeof(buf), "%d orders  %d units", t->orders, t->units);
    oled_text(0, 31, buf, 1, true);

    snprintf(buf, sizeof(buf), "%d shipped", t->shipped);
    oled_text(0, 42, buf, 1, true);
    if (t->newest_name[0]) {
        oled_text(OLED_WIDTH - oled_text_width(t->newest_name, 1), 42, t->newest_name, 1, true);
    }
}

static void page_revenue(const shopify_today_t *t)
{
    char money[32], buf[48];
    draw_header("REVENUE");

    format_money(t->revenue_cents, money, sizeof(money));
    draw_big_centered(16, money, 3);

    if (t->orders > 0) {
        format_money(t->revenue_cents / t->orders, money, sizeof(money));
        snprintf(buf, sizeof(buf), "avg %s / order", money);
        oled_text_centered(42, buf, 1, true);
    }
}

static void page_units(const shopify_today_t *t)
{
    char buf[16];
    draw_header("UNITS SOLD");
    snprintf(buf, sizeof(buf), "%d", t->units);
    draw_big_centered(16, buf, 4);
}

static void page_shipped(const shopify_today_t *t)
{
    char buf[32];
    draw_header("SHIPPED");

    snprintf(buf, sizeof(buf), "%d/%d", t->shipped, t->orders);
    draw_big_centered(14, buf, 3);

    int left = t->orders - t->shipped;
    if (t->orders == 0) {
        oled_text_centered(42, "no orders yet", 1, true);
    } else if (left == 0) {
        oled_text_centered(42, "all shipped!", 1, true);
    } else {
        snprintf(buf, sizeof(buf), "%d left to ship", left);
        oled_text_centered(42, buf, 1, true);
    }
}

// Handy for debugging a device that's been running for days.
static void page_device(void)
{
    char buf[64];
    draw_header("DEVICE");

    wifi_ap_record_t ap;
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(buf, sizeof(buf), "wifi %d dBm", ap.rssi);
    } else {
        snprintf(buf, sizeof(buf), "wifi down");
    }
    oled_text(0, 12, buf, 1, true);

    long long up = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf), "up %lldd %02lld:%02lld", up / 86400, (up % 86400) / 3600, (up % 3600) / 60);
    oled_text(0, 22, buf, 1, true);

    snprintf(buf, sizeof(buf), "heap min %uk", (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    oled_text(0, 32, buf, 1, true);

    snprintf(buf, sizeof(buf), "fw %.16s", esp_app_get_description()->version);
    oled_text(0, 42, buf, 1, true);
}

// Redraws the whole screen from the current state. Safe to call from any task.
static void ui_refresh(void)
{
    if (!s_oled_ok) {
        return;
    }
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    oled_clear();

    if (!s_have_data) {
        oled_text_centered(3, "ORDER NOTIFIER", 1, true);
        oled_hline(0, 13, OLED_WIDTH);
        oled_text_centered(30, s_status, 1, true);
    } else {
        switch (s_page) {
        case PAGE_REVENUE: page_revenue(&s_today); break;
        case PAGE_UNITS:   page_units(&s_today);   break;
        case PAGE_SHIPPED: page_shipped(&s_today); break;
        case PAGE_DEVICE:  page_device();          break;
        default:           page_overview(&s_today); break;
        }
        draw_footer();
    }

    oled_flush();
    xSemaphoreGive(s_ui_mutex);
}

static void ui_set_status(const char *msg)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    strlcpy(s_status, msg, sizeof(s_status));
    xSemaphoreGive(s_ui_mutex);
    ui_refresh();
}

// Drawn directly during an update, without touching the page state.
static void ui_ota(int percent, const char *msg)
{
    if (!s_oled_ok) {
        return;
    }
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    oled_clear();
    oled_text_centered(3, "UPDATING", 1, true);
    oled_hline(0, 13, OLED_WIDTH);
    oled_text_centered(19, msg, 1, true);
    if (percent >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        oled_text_centered(30, buf, 2, true);
        // Progress bar: hollow outline with a solid fill inside.
        oled_fill_rect(0, 50, OLED_WIDTH, 8, true);
        oled_fill_rect(1, 51, OLED_WIDTH - 2, 6, false);
        oled_fill_rect(1, 51, (OLED_WIDTH - 2) * percent / 100, 6, true);
    }
    oled_flush();
    xSemaphoreGive(s_ui_mutex);
}

// OTA runs in its own task: it needs a big stack for TLS and takes a while.
static void ota_task(void *arg)
{
    esp_err_t err = ota_check_and_apply(ui_ota);  // reboots itself on success

    if (err == ESP_ERR_NOT_FOUND) {
        ui_ota(-1, "up to date");
    } else {
        ESP_LOGE(TAG, "Update failed: %s", esp_err_to_name(err));
        ui_ota(-1, "update failed");
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_ota_busy = false;
    xSemaphoreGive(s_ui_mutex);
    ui_refresh();
    vTaskDelete(NULL);
}

// ---------- Button ----------

static void on_button_press(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_last_press_us = esp_timer_get_time();
    if (s_unread) {
        s_unread = false;  // first press acknowledges the new order
    } else if (s_have_data) {
        s_page = (page_t)((s_page + 1) % PAGE_COUNT);
    }
    bool ring = s_unread;
    xSemaphoreGive(s_ui_mutex);

    button_set_ring(ring);
    ui_refresh();
}

static void on_button_long_press(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    bool already = s_ota_busy;
    s_ota_busy = true;
    xSemaphoreGive(s_ui_mutex);
    if (already) {
        return;
    }
    xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
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

    // Fetch into a local copy first, so the screen never sees half-updated data.
    shopify_today_t fresh;
    esp_err_t err = shopify_fetch_today(s_token, since, s_last_seen, &fresh);
    if (err == SHOPIFY_ERR_AUTH) {
        s_token_refresh_at_us = 0;  // force a new token on the next attempt
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }

    if (fresh.newest_created[0] && strcmp(fresh.newest_created, s_last_seen) > 0) {
        strlcpy(s_last_seen, fresh.newest_created, sizeof(s_last_seen));
    }

    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_today = fresh;
    s_have_data = true;
    s_online = true;
    if (fresh.new_orders > 0) {
        s_unread = true;
        s_page = PAGE_OVERVIEW;
    }
    if (s_page != PAGE_OVERVIEW && now_us - s_last_press_us > PAGE_TIMEOUT_S * 1000000LL) {
        s_page = PAGE_OVERVIEW;
    }
    bool ring = s_unread;
    xSemaphoreGive(s_ui_mutex);

    if (fresh.new_orders > 0) {
        ESP_LOGW(TAG, "*** %d NEW ORDER%s! Latest: %s ***",
                 fresh.new_orders, fresh.new_orders > 1 ? "S" : "", fresh.newest_name);
        audio_chime();
        leds_new_order();
    }
    button_set_ring(ring);

    long long cents = fresh.revenue_cents;
    ESP_LOGI(TAG, "Today: %d orders | %lld.%02lld %s | %d units | %d shipped | heap %u (min %u)",
             fresh.orders, cents / 100, (cents < 0 ? -cents : cents) % 100, fresh.currency,
             fresh.units, fresh.shipped,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    return ESP_OK;
}

static void notifier_task(void *arg)
{
    ui_set_status("connecting to wifi...");
    if (wifi_init_and_connect() != ESP_OK) {
        ESP_LOGW(TAG, "Initial Wi-Fi connection failed, will keep retrying");
    }

    ui_set_status("syncing clock...");
    time_sync_start();

    ui_set_status("checking orders...");

    int backoff_s = BACKOFF_START_S;
    for (;;) {
        int wait_s;
        if (poll_once() == ESP_OK) {
            // A successful poll means Wi-Fi, TLS and Shopify all work, which is
            // exactly the self-test a freshly installed firmware has to pass.
            if (s_pending_verify) {
                ota_mark_valid();
                s_pending_verify = false;
            }
            leds_set_state(LEDS_OK);
            ui_refresh();
            backoff_s = BACKOFF_START_S;
            wait_s = POLL_INTERVAL_S;
        } else {
            wait_s = backoff_s;
            ESP_LOGW(TAG, "Poll failed, retrying in %d s", wait_s);

            if (s_pending_verify && ++s_selftest_fails >= SELFTEST_MAX_FAILS) {
                ota_rollback_now();  // reboots into the previous firmware
            }

            xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
            s_online = false;
            bool have_data = s_have_data;
            xSemaphoreGive(s_ui_mutex);

            leds_set_state(have_data ? LEDS_OFFLINE : LEDS_CONNECTING);
            if (have_data) {
                ui_refresh();  // keep showing the last stats, marked OFFLINE
            } else {
                ui_set_status("can't reach shopify");
            }
            backoff_s = (backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff_s * 2;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }
}

void app_main(void)
{
    // NVS holds the credentials (and Wi-Fi calibration data).
    // Deliberately NOT erased on failure: erasing would wipe the provisioned credentials.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s). Re-flash the credentials image.", esp_err_to_name(err));
    }

    s_ui_mutex = xSemaphoreCreateMutex();

    // Each peripheral is optional: a broken one shouldn't stop the others.
    s_oled_ok = (oled_init() == ESP_OK);
    if (!s_oled_ok) {
        ESP_LOGE(TAG, "OLED init failed, continuing without display");
    }

    if (audio_init() == ESP_OK) {
        audio_chime();  // boot test so you know the speaker works
    } else {
        ESP_LOGE(TAG, "Audio init failed, continuing without sound");
    }

    if (leds_init() != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed, continuing without lights");
    }

    s_pending_verify = ota_is_pending_verify();
    if (s_pending_verify) {
        ESP_LOGW(TAG, "Running new firmware on trial: it must fetch orders once to be kept");
    }

    if (button_init(on_button_press, on_button_long_press) != ESP_OK) {
        ESP_LOGE(TAG, "Button init failed, continuing without it");
    }

    if (err != ESP_OK || creds_load() != ESP_OK) {
        // Without credentials there's nothing to do: say so on the screen and stop.
        ui_set_status("not provisioned");
        leds_set_state(LEDS_OFFLINE);
        return;
    }

    xTaskCreate(notifier_task, "notifier", 8192, NULL, 5, NULL);
}