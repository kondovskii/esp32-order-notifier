#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "shopify.h"
#include "secrets.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

#define API_VERSION   "2026-07"
#define RESP_BUF_SIZE 8192   // probe showed ~1.2 KB for 5 orders, so lots of headroom

static const char *TAG = "shopify";
static char s_resp[RESP_BUF_SIZE];

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} resp_t;

// The HTTP client hands us the response body in pieces. Collect them into one buffer.
static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_t *r = (resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r) {
        if (r->len + evt->data_len < r->cap) {
            memcpy(r->buf + r->len, evt->data, evt->data_len);
            r->len += evt->data_len;
            r->buf[r->len] = '\0';
        } else {
            r->overflow = true;
        }
    }
    return ESP_OK;
}

// One HTTPS POST with real certificate verification via the CA bundle.
static esp_err_t https_post(const char *url, const char *content_type, const char *token,
                            const char *body, int *status_out, resp_t *resp)
{
    resp->buf = s_resp;
    resp->len = 0;
    resp->cap = sizeof(s_resp);
    resp->overflow = false;
    s_resp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event,
        .user_data = resp,
        .crt_bundle_attach = esp_crt_bundle_attach,  // verify the server's certificate
        .timeout_ms = 15000,
        .buffer_size = 4096,     // Shopify sends long response headers
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", content_type);
    if (token) {
        esp_http_client_set_header(client, "X-Shopify-Access-Token", token);
    }
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    *status_out = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (resp->overflow) {
        ESP_LOGE(TAG, "Response bigger than %d byte buffer", RESP_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "?";
}

esp_err_t shopify_get_token(char *token_out, size_t token_len, int *expires_in)
{
    char url[128];
    snprintf(url, sizeof(url), "https://%s.myshopify.com/admin/oauth/access_token", SHOPIFY_SHOP);

    char body[256];
    snprintf(body, sizeof(body),
             "grant_type=client_credentials&client_id=%s&client_secret=%s",
             SHOPIFY_CLIENT_ID, SHOPIFY_CLIENT_SECRET);

    ESP_LOGI(TAG, "Requesting access token from %s.myshopify.com ...", SHOPIFY_SHOP);
    resp_t resp;
    int status = 0;
    esp_err_t err = https_post(url, "application/x-www-form-urlencoded", NULL, body, &status, &resp);
    memset(body, 0, sizeof(body));  // wipe the secret from memory
    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Token request failed, HTTP %d: %.200s", status, resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    cJSON *tok = cJSON_GetObjectItem(root, "access_token");
    cJSON *exp = cJSON_GetObjectItem(root, "expires_in");
    if (!cJSON_IsString(tok) || strlen(tok->valuestring) >= token_len) {
        ESP_LOGE(TAG, "No usable access_token in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strlcpy(token_out, tok->valuestring, token_len);
    if (expires_in) {
        *expires_in = cJSON_IsNumber(exp) ? exp->valueint : 0;
    }
    cJSON_Delete(root);
    memset(s_resp, 0, sizeof(s_resp));  // the raw response also contained the token
    return ESP_OK;
}

esp_err_t shopify_print_recent_orders(const char *token)
{
    char url[128];
    snprintf(url, sizeof(url), "https://%s.myshopify.com/admin/api/" API_VERSION "/graphql.json",
             SHOPIFY_SHOP);

    // Ask only for the fields we display, to keep the response small.
    const char *query =
        "{ orders(first: 5, sortKey: CREATED_AT, reverse: true) { edges { node {"
        " name createdAt displayFinancialStatus"
        " totalPriceSet { shopMoney { amount currencyCode } } } } } }";

    // Build {"query": "..."} with cJSON so quotes are escaped correctly.
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "query", query);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Fetching recent orders ...");
    resp_t resp;
    int status = 0;
    esp_err_t err = https_post(url, "application/json", token, body, &status, &resp);
    cJSON_free(body);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "HTTP %d, response size: %u bytes", status, (unsigned)resp.len);
    if (status != 200) {
        ESP_LOGE(TAG, "Orders request failed: %.200s", resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    if (!root) {
        ESP_LOGE(TAG, "Could not parse JSON response");
        return ESP_FAIL;
    }
    cJSON *errors = cJSON_GetObjectItem(root, "errors");
    if (errors) {
        char *e = cJSON_PrintUnformatted(errors);
        ESP_LOGE(TAG, "GraphQL errors: %.300s", e ? e : "?");
        cJSON_free(e);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *orders = cJSON_GetObjectItem(data, "orders");
    cJSON *edges = cJSON_GetObjectItem(orders, "edges");
    cJSON *edge;
    cJSON_ArrayForEach(edge, edges) {
        cJSON *node = cJSON_GetObjectItem(edge, "node");
        cJSON *money = cJSON_GetObjectItem(cJSON_GetObjectItem(node, "totalPriceSet"), "shopMoney");
        printf("  %-7s  %s  %s %s  %s\n",
               json_str(node, "name"), json_str(node, "createdAt"),
               json_str(money, "amount"), json_str(money, "currencyCode"),
               json_str(node, "displayFinancialStatus"));
    }

    cJSON_Delete(root);
    return ESP_OK;
}