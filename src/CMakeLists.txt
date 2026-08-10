idf_component_register(
    SRCS "main.c" "router_config.c" "web_server.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip driver
             esp_http_server mbedtls esp_timer
)
