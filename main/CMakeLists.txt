idf_component_register(
    SRCS "atari8EspBusBang.cpp" "arduinoLite.cpp" "core1.cpp" "wifi.cpp" "extMem.cpp"
    PRIV_REQUIRES 
        spi_flash esp_driver_gpio esp_partition esp_timer 
        hal esp_driver_ledc esp_driver_uart esp_driver_usb_serial_jtag
        spiffs
        esp_wifi nvs_flash
        esp_http_server
        libsmb2
    INCLUDE_DIRS "/home/jim/esp/esp-idf/components/spiffs/spiffs/src/" 
    
    # Use DRAM_ATTR and IRAM_ATTR instead of placing the entire project in SRAM
    # Better granularity and control frees up more SRAM
    #LDFRAGMENTS "./link.lf" 
)
target_compile_options(${COMPONENT_LIB} PRIVATE -mno-serialize-volatile)
if(DEFINED PROFILEMODE) 
    target_compile_options(${COMPONENT_LIB} PRIVATE -DPROF${PROFILEMODE})
    target_compile_options(${COMPONENT_LIB} PRIVATE -DTEST_SEC=${TEST_SEC})
endif()

if (DEFINED BOOT_SDX)
    target_compile_options(${COMPONENT_LIB} PRIVATE -DBOOT_SDX=${BOOT_SDX})
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -ffunction-sections)


