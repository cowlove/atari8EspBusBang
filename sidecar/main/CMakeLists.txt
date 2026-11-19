idf_component_register(
    SRCS "main.cpp" "sidecarApp.cpp" "sidecarSampler.cpp" "mainReset.cpp" "activityLed.cpp"
    PRIV_REQUIRES bus_profiler esp_driver_gpio esp_driver_ledc esp_driver_uart esp_rom sidecar_link disk_image spiffs esp_timer fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_driver_spi esp_driver_rmt mbedtls
)

# Experiment knobs from tools/boot_sidecar.sh's SIDECAR_DEFINES (e.g.
# SIDECAR_DEFINES=-DSLOT_TUNE=0): cache variables, cleared by boot_sidecar.sh
# when not given.
foreach(define_name SLOT_TUNE SLOT_FAST_US SLOT_TRACE)
    if(DEFINED ${define_name})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "${define_name}=${${define_name}}")
    endif()
endforeach()
