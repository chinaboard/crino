idf_component_register(
    SRCS "cr_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mbedtls log
)
