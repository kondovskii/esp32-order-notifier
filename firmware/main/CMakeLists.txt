idf_component_register(
    SRCS "main.c" "wifi.c" "shopify.c" "time_sync.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer esp_http_client mbedtls json
)