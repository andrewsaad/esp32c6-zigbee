idf_component_register(
    SRCS "src/zb_router.c" "src/zb_router_epid.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES nvs_flash esp_driver_gpio freertos log
)
