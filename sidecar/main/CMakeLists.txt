idf_component_register(
    SRCS "main.cpp" "sidecarApp.cpp" "sidecarSampler.cpp" "mainReset.cpp" "activityLed.cpp"
         "sidecarNet.cpp" "sidecarTelnet.cpp"
    PRIV_REQUIRES bus_profiler esp_driver_gpio esp_driver_ledc esp_driver_uart esp_rom sidecar_link disk_image spiffs esp_timer fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_driver_spi esp_driver_rmt mbedtls
                  # The network (sidecarNet.cpp, sidecarTelnet.cpp): main has none.
                  esp_wifi esp_netif esp_event nvs_flash esp_http_server lwip igrr__rfc2217-server
)
