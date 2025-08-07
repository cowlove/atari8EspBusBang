idf_component_register(
    SRCS "atari8EspBusBang.cpp" "arduinoLite.cpp" "core1.cpp" "littlefs/lfs.c" "littlefs/lfs_util.c"
    PRIV_REQUIRES 
        spi_flash esp_driver_gpio esp_partition esp_timer 
        hal esp_driver_ledc esp_driver_uart esp_driver_usb_serial_jtag
    INCLUDE_DIRS "./littlefs")

target_compile_options(${COMPONENT_LIB} PRIVATE -mno-serialize-volatile)

if(DEFINED PROFILEMODE) 
    target_compile_options(${COMPONENT_LIB} PRIVATE -DPROF${PROFILEMODE})
    target_compile_options(${COMPONENT_LIB} PRIVATE -DTEST_SEC=30)
endif()

#target_compile_options(${COMPONENT_LIB} PRIVATE \${DEFS})


