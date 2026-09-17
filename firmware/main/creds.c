#include <string.h>
#include "creds.h"
#include "nvs.h"
#include "esp_log.h"

#define NAMESPACE "creds"   // must match the namespace row in the CSV

static const char *TAG = "creds";
static creds_t s_creds;

static esp_err_t read_str(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Missing key '%s'", key);
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "Value for '%s' is too long", key);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reading '%s' failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t creds_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No '%s' namespace in NVS (%s). Has the device been provisioned?",
                 NAMESPACE, esp_err_to_name(err));
        return err;
    }

    // Stop at the first failure; each read_str logs which key was the problem.
    err = read_str(h, "wifi_ssid", s_creds.wifi_ssid, sizeof(s_creds.wifi_ssid));
    if (err == ESP_OK) err = read_str(h, "wifi_pass", s_creds.wifi_pass, sizeof(s_creds.wifi_pass));
    if (err == ESP_OK) err = read_str(h, "shop", s_creds.shop, sizeof(s_creds.shop));
    if (err == ESP_OK) err = read_str(h, "client_id", s_creds.client_id, sizeof(s_creds.client_id));
    if (err == ESP_OK) err = read_str(h, "client_secret", s_creds.client_secret, sizeof(s_creds.client_secret));
    nvs_close(h);

    if (err != ESP_OK) {
        memset(&s_creds, 0, sizeof(s_creds));  // don't leave partial secrets in RAM
        return err;
    }

    // Log only non-secret values.
    ESP_LOGI(TAG, "Loaded credentials for Wi-Fi '%s' and shop '%s'", s_creds.wifi_ssid, s_creds.shop);
    return ESP_OK;
}

const creds_t *creds_get(void)
{
    return &s_creds;
}