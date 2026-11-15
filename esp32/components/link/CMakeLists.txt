idf_component_register(
    SRCS "link.c"
    INCLUDE_DIRS "include"
    REQUIRES proto driver esp_timer freertos
)
