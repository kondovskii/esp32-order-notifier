#include <string.h>
#include "ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"

// The "latest" URL always points at the newest published release.
// raw.githubusercontent.com serves files directly, with no redirect.
#define MANIFEST_URL "https://raw.githubusercontent.com/kondovskii/esp32-order-notifier/main/manifest.json"
#define MANIFEST_MAX 1024

static const char *TAG = "ota";
static char s_manifest[MANIFEST_MAX];
static size_t s_manifest_len;

static esp_err_t manifest_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_manifest_len + evt->data_len < sizeof(s_manifest)) {
            memcpy(s_manifest + s_manifest_len, evt->data, evt->data_len);
            s_manifest_len += evt->data_len;
            s_manifest[s_manifest_len] = '\0';
        }
    }
    return ESP_OK;
}

// Downloads the small JSON file describing the newest release.
static esp_err_t fetch_manifest(char *version, size_t version_len, char *url, size_t url_len)
{
    s_manifest_len = 0;
    s_manifest[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = MANIFEST_URL,
        .event_handler = manifest_event,
        .crt_bundle_attach = esp_crt_bundle_attach,  // verify GitHub's certificate
        .timeout_ms = 15000,
        .buffer_size = 8192,   // GitHub's redirect Location header is very long
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Manifest request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Manifest HTTP %d", status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(s_manifest);
    cJSON *v = cJSON_GetObjectItem(root, "version");
    cJSON *u = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(v) || !cJSON_IsString(u)) {
        ESP_LOGE(TAG, "Manifest is missing 'version' or 'url'");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strlcpy(version, v->valuestring, version_len);
    strlcpy(url, u->valuestring, url_len);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ota_check_and_apply(ota_progress_cb_t cb)
{
    char new_version[32] = {0};
    char url[256] = {0};

    if (cb) {
        cb(-1, "checking...");
    }
    esp_err_t err = fetch_manifest(new_version, sizeof(new_version), url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }

    const char *running = esp_app_get_description()->version;
    ESP_LOGI(TAG, "Running %s, latest release is %s", running, new_version);
    if (strcmp(running, new_version) == 0) {
        return ESP_ERR_NOT_FOUND;  // nothing to do
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 8192,      // same long redirect applies to the firmware download
        .buffer_size_tx = 2048,
    };

    esp_https_ota_config_t ota_cfg = {.http_config = &http_cfg};

    esp_https_ota_handle_t handle = NULL;
    err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }

    // Total size comes from the image header, so progress can be a percentage.
    int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "Downloading %d bytes into the spare OTA slot", total);

    int last_shown = -10;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (cb && total > 0) {
            int pct = esp_https_ota_get_image_len_read(handle) * 100 / total;
            if (pct >= last_shown + 2) {  // don't redraw for every chunk
                last_shown = pct;
                cb(pct, new_version);
            }
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "Download incomplete: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    // finish() checks the image is valid and marks the new slot as the one to boot.
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Update installed, restarting into %s", new_version);
    if (cb) {
        cb(100, "restarting");
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;  // never reached
}

bool ota_is_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_mark_valid(void)
{
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "New firmware verified, rollback cancelled");
    }
}

void ota_rollback_now(void)
{
    ESP_LOGE(TAG, "New firmware failed its self-test, rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}