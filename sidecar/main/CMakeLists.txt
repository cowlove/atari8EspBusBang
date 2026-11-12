idf_component_register(
    SRCS "main.cpp" "sidecarApp.cpp"
    PRIV_REQUIRES sidecar_link disk_image spiffs esp_timer fatfs sdmmc esp_driver_sdmmc
)
