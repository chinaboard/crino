idf_component_register(
    SRCS "cr_session.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer log
    PRIV_REQUIRES esp_system
)
