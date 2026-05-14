idf_component_register(
    SRCS "cr_captive.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip log
)
