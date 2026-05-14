idf_component_register(
    SRCS "cr_led.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer log
    PRIV_REQUIRES wifi_mgr time_sync esp_system nvs_flash esp_driver_ledc
)
