idf_component_register(
    SRCS "main.c" "wifi.c" "shopify.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client mbedtls json
)