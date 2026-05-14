idf_component_register(
    SRCS "smoketest.c"
    INCLUDE_DIRS ""
    REQUIRES esp_http_server esp_timer log http_app led
    WHOLE_ARCHIVE TRUE
)
