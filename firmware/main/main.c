#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "wifi.h"
#include "shopify.h"

static const char *TAG = "main";

// All the network work runs in its own task with an 8 KB stack.
// TLS uses a lot of stack, and app_main's default stack is too small for it.
static void notifier_task(void *arg)
{
    static char token[128];  // static = kept off the stack
    int expires_in = 0;

    if (wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection failed");
        vTaskDelete(NULL);
        return;
    }

    if (shopify_get_token(token, sizeof(token), &expires_in) != ESP_OK) {
        ESP_LOGE(TAG, "Could not get Shopify access token");
        vTaskDelete(NULL);
        return;
    }
    // Never log the token itself.
    ESP_LOGI(TAG, "Got access token (expires in %d s)", expires_in);

    shopify_print_recent_orders(token);

    ESP_LOGI(TAG, "Free heap: %u bytes (lowest ever: %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());

    vTaskDelete(NULL);
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