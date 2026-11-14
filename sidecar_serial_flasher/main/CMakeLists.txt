idf_component_register(
    SRCS "main.cpp" "mainFlasher.cpp"
    PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_uart esp_rom esp_timer spiffs
)
