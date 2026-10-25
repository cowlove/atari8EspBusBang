set(MAIN_PRIV_REQUIRES
    spi_flash esp_driver_gpio esp_partition esp_timer
    hal esp_driver_ledc esp_driver_uart esp_driver_usb_serial_jtag
    esp_driver_rmt driver
    spiffs esp_rom
    esp_wifi nvs_flash
    esp_http_server
    libsmb2
    igrr__rfc2217-server
)

if(CONFIG_A8BB_ENABLE_MDNS)
    list(APPEND MAIN_PRIV_REQUIRES espressif__mdns)
endif()

idf_component_register(
    SRCS "main.cpp" "arduinoLite.cpp" "core1.cpp" "wifi.cpp"
        "pbirom.h" "telnet.cpp" "diskFlash.cpp" "cartridge.cpp" "gzipImage.cpp"
        "sysMonitor.cpp" "sysConfig.cpp"
        # "mmu.cpp" "bmon.cpp" "pbi.cpp" "sio.cpp" "cio.cpp" "extMem.cpp" "arduinoLite.cpp" "cartridge.cpp" "core1.cpp" 
    PRIV_REQUIRES ${MAIN_PRIV_REQUIRES}

    INCLUDE_DIRS "${IDF_PATH}/components/spiffs/spiffs/src/"
    
    # Use DRAM_ATTR and IRAM_ATTR instead of placing the entire project in SRAM
    # Better granularity and control frees up more SRAM
    LDFRAGMENTS "./link.lf"
)
add_custom_command(
    OUTPUT ${COMPONENT_DIR}/pbirom.h
    COMMAND ${CMAKE_COMMAND} -E echo ${COMPONENT_DIR} "Generating header file..."
    COMMAND make -C "${COMPONENT_DIR}" pbirom.h
    DEPENDS ${COMPONENT_DIR}/pbirom.asm ${COMPONENT_DIR}/asmDefs.h
)

target_compile_options(${COMPONENT_LIB} PRIVATE -mno-serialize-volatile)
set(A8BB_COMPILE_OPTIONS)
if(DEFINED PROFILEMODE)
    list(APPEND A8BB_COMPILE_OPTIONS "-DPROF${PROFILEMODE}")
    if(DEFINED TEST_SEC)
        list(APPEND A8BB_COMPILE_OPTIONS "-DTEST_SEC=${TEST_SEC}")
    endif()
endif()
foreach(define_name BOOT_CONFIG HW_TYPE RUNTIME_SEC BOOT_SDX BUS_ANALYZER)
    if(DEFINED ${define_name})
        list(APPEND A8BB_COMPILE_OPTIONS "-D${define_name}=${${define_name}}")
    endif()
endforeach()
target_compile_options(${COMPONENT_LIB} PRIVATE ${A8BB_COMPILE_OPTIONS})

target_compile_options(${COMPONENT_LIB} PRIVATE -ffunction-sections)
