idf_component_register(
    # gzipStream: the streaming gunzip for app.bin.gz, compiled in from
    # components/disk_image (the whole component would pull in libsmb2).
    SRCS "main.cpp" "mainFlasher.cpp" "../../components/disk_image/gzipStream.cpp"
    INCLUDE_DIRS "../../components/disk_image"
    PRIV_REQUIRES heap esp_driver_gpio esp_driver_ledc esp_driver_uart esp_rom esp_timer spiffs
)
