idf_component_register(
    SRCS "main.cpp" "sidecarApp.cpp" "consoleTee.c" "sidecarSampler.cpp" "mainReset.cpp" "atariReset.cpp" "activityLed.cpp" "mainFlasher.cpp"
         "sidecarNet.cpp" "sidecarTelnet.cpp" "sidecarWeb.cpp"
         # The boot profiles, shared with main: both apps load BOOT_CONFIG's.
         "../../main/sysConfig.cpp"
    PRIV_REQUIRES bus_profiler esp_driver_gpio esp_driver_ledc esp_driver_uart esp_rom sidecar_link disk_image spiffs esp_timer fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_driver_spi esp_driver_rmt mbedtls app_update esp_app_format esp_partition
                  # The network (sidecarNet.cpp, sidecarTelnet.cpp, sidecarWeb.cpp).
                  esp_wifi esp_netif esp_event nvs_flash esp_http_server lwip igrr__rfc2217-server
)

# BOOT_CONFIG (-DBOOT_CONFIG=\"NAME\"): the boot profile, the same one main
# builds with (tools/boot_sidecar.sh passes main's); empty: the default.
if(DEFINED BOOT_CONFIG)
    target_compile_options(${COMPONENT_LIB} PRIVATE "-DBOOT_CONFIG=${BOOT_CONFIG}")   # as main/CMakeLists.txt passes it
endif()

# Experiment knobs from tools/boot_sidecar.sh's SIDECAR_DEFINES (e.g.
# SIDECAR_DEFINES=-DSLOT_TUNE=0): cache variables, cleared by boot_sidecar.sh
# when not given.
foreach(define_name SLOT_TUNE SLOT_FAST_US SLOT_TRACE ERASE_CORE I2SLINK_STATS_LOG BOOT_TOKEN TEST_UNHEALTHY)
    if(DEFINED ${define_name})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "${define_name}=${${define_name}}")
    endif()
endforeach()
