#include <string.h>
#include "wifi.h"
#include "creds.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRIES        5

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static int s_retries = 0;

// Wi-Fi is event-driven: the driver calls this function when things happen.
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        if (s_retries < MAX_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "Disconnected, retry %d/%d", s_retries, MAX_RETRIES);
            esp_wifi_connect();
        } else {
            // Stop retrying on our own; the main loop decides when to try again.
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retries = 0;
        xEventGroupClearBits(s_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wait_for_result(TickType_t timeout)
{
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, timeout);
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_init_and_connect(void)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wifi_cfg = {0};
    const creds_t *creds = creds_get();
    strlcpy((char *)wifi_cfg.sta.ssid, creds->wifi_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, "deliberately-wrong", sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Log the network name only, never the password.
    ESP_LOGI(TAG, "Connecting to %s ...", creds->wifi_ssid);
    return wait_for_result(portMAX_DELAY);
}

bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_reconnect(int timeout_ms)
{
    if (wifi_is_connected()) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Reconnecting to %s ...", creds_get()->wifi_ssid);
    s_retries = 0;
    xEventGroupClearBits(s_events, WIFI_FAIL_BIT);
    esp_wifi_connect();
    return wait_for_result(pdMS_TO_TICKS(timeout_ms));
}