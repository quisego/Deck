idf_component_register(
    SRCS "bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES proto link esp_wifi esp_netif esp_event nvs_flash mqtt esp_timer
)
