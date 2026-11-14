idf_component_register(
    SRCS "main.cpp" "sidecarApp.cpp" "sidecarSampler.cpp" "mainFlasher.cpp"
    PRIV_REQUIRES bus_profiler esp_driver_gpio esp_driver_ledc esp_driver_uart esp_rom sidecar_link disk_image spiffs esp_timer fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_driver_spi mbedtls
)
