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
#define RESP_BUF_SIZE 16384   // room for ~50 orders in one response

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
    return cJSON_IsString(item) ? item->valuestring : "";
}

// Turns "79.94" into 7994 without using floating point.
static int64_t amount_to_cents(const char *s)
{
    bool negative = false;
    int64_t whole = 0, frac = 0;
    int frac_digits = 0;

    if (*s == '-') {
        negative = true;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        whole = whole * 10 + (*s++ - '0');
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && frac_digits < 2) {
            frac = frac * 10 + (*s++ - '0');
            frac_digits++;
        }
    }
    if (frac_digits == 1) {
        frac *= 10;  // "45.2" means 45.20
    }
    int64_t cents = whole * 100 + frac;
    return negative ? -cents : cents;
}

esp_err_t shopify_get_token(char *token_out, size_t token_len, int *expires_in)
{
    char url[128];
    snprintf(url, sizeof(url), "https://%s.myshopify.com/admin/oauth/access_token", SHOPIFY_SHOP);

    char body[256];
    snprintf(body, sizeof(body),
             "grant_type=client_credentials&client_id=%s&client_secret=%s",
             SHOPIFY_CLIENT_ID, SHOPIFY_CLIENT_SECRET);

    ESP_LOGI(TAG, "Requesting access token ...");
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

esp_err_t shopify_fetch_today(const char *token, const char *since_utc,
                              const char *last_seen, shopify_today_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->currency, "CAD", sizeof(out->currency));

    char url[128];
    snprintf(url, sizeof(url), "https://%s.myshopify.com/admin/api/" API_VERSION "/graphql.json",
             SHOPIFY_SHOP);

    // Newest first, only today's orders, only the fields we use.
    // The date filter is passed as a GraphQL variable ($q) so it needs no escaping.
    const char *query =
        "query($q: String) { orders(first: 50, sortKey: CREATED_AT, reverse: true, query: $q) {"
        " pageInfo { hasNextPage }"
        " nodes { name createdAt cancelledAt displayFulfillmentStatus subtotalLineItemsQuantity"
        " totalPriceSet { shopMoney { amount currencyCode } } } } }";

    char filter[64];
    snprintf(filter, sizeof(filter), "created_at:>=%s", since_utc);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "query", query);
    cJSON *vars = cJSON_AddObjectToObject(req, "variables");
    cJSON_AddStringToObject(vars, "q", filter);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    resp_t resp;
    int status = 0;
    esp_err_t err = https_post(url, "application/json", token, body, &status, &resp);
    cJSON_free(body);
    if (err != ESP_OK) {
        return err;
    }
    if (status == 401) {
        ESP_LOGW(TAG, "Access token rejected (HTTP 401)");
        return SHOPIFY_ERR_AUTH;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Orders request failed, HTTP %d: %.200s", status, resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    if (!root) {
        ESP_LOGE(TAG, "Could not parse JSON response");
        return ESP_FAIL;
    }
    cJSON *errors = cJSON_GetObjectItem(root, "errors");
    if (errors) {
        // Includes rate limiting ("THROTTLED"); the main loop will back off and retry.
        char *e = cJSON_PrintUnformatted(errors);
        ESP_LOGE(TAG, "GraphQL errors: %.300s", e ? e : "?");
        cJSON_free(e);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *orders = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "orders");
    cJSON *page = cJSON_GetObjectItem(orders, "pageInfo");
    out->truncated = cJSON_IsTrue(cJSON_GetObjectItem(page, "hasNextPage"));

    cJSON *order;
    cJSON_ArrayForEach(order, cJSON_GetObjectItem(orders, "nodes")) {
        if (cJSON_IsString(cJSON_GetObjectItem(order, "cancelledAt"))) {
            continue;  // skip cancelled orders
        }

        const char *created = json_str(order, "createdAt");
        cJSON *money = cJSON_GetObjectItem(cJSON_GetObjectItem(order, "totalPriceSet"), "shopMoney");

        if (out->orders == 0) {
            // Results are newest first, so the first one is the newest.
            strlcpy(out->newest_name, json_str(order, "name"), sizeof(out->newest_name));
            strlcpy(out->newest_created, created, sizeof(out->newest_created));
            const char *cur = json_str(money, "currencyCode");
            if (cur[0]) {
                strlcpy(out->currency, cur, sizeof(out->currency));
            }
        }

        out->orders++;
        out->revenue_cents += amount_to_cents(json_str(money, "amount"));

        cJSON *qty = cJSON_GetObjectItem(order, "subtotalLineItemsQuantity");
        if (cJSON_IsNumber(qty)) {
            out->units += qty->valueint;
        }
        if (strcmp(json_str(order, "displayFulfillmentStatus"), "FULFILLED") == 0) {
            out->shipped++;
        }
        // ISO timestamps in the same format sort correctly as plain strings.
        if (last_seen[0] && strcmp(created, last_seen) > 0) {
            out->new_orders++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}