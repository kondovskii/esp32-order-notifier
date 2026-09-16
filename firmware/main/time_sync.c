#include <stdlib.h>
#include "time_sync.h"
#include "freertos/FreeRTOS.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"

// Eastern time with daylight saving: EST (UTC-5) normally,
// EDT from the 2nd Sunday of March to the 1st Sunday of November.
#define LOCAL_TZ "EST5EDT,M3.2.0,M11.1.0"

static const char *TAG = "time";

esp_err_t time_sync_start(void)
{
    setenv("TZ", LOCAL_TZ, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Waiting for time sync ...");
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
        char buf[40];
        time_format_local(time(NULL), buf, sizeof(buf));
        ESP_LOGI(TAG, "Clock set: %s", buf);
    } else {
        // SNTP keeps retrying in the background; the main loop checks time_is_valid().
        ESP_LOGW(TAG, "Time not synced yet, still trying in background");
    }
    return ESP_OK;
}

bool time_is_valid(void)
{
    // Before sync the clock starts at 1970. Anything after 2025-01-01 means it's been set.
    return time(NULL) > 1735689600;
}

void time_format_utc(time_t t, char *out, size_t len)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void time_format_local(time_t t, char *out, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, len, "%Y-%m-%d %H:%M:%S %Z", &tm);
}

time_t time_start_of_local_day(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;  // let mktime work out whether DST applied at midnight
    return mktime(&tm);
}