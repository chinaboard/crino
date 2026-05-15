idf_component_register(
    SRCS "cr_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_timer lwip log config util
    PRIV_REQUIRES captive_portal esp_app_format
)
