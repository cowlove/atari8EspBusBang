idf_component_register(
    SRCS "atari8EspBusBang.cpp" "core1.cpp" "littlefs/lfs.c" "littlefs/lfs_util.c"
    PRIV_REQUIRES spi_flash esp_driver_gpio esp_partition esp_timer hal esp_driver_ledc
    INCLUDE_DIRS "./littlefs")

target_compile_options(${COMPONENT_LIB} PRIVATE -mno-serialize-volatile)
#target_compile_options(${COMPONENT_LIB} PRIVATE -DPROFB)
#target_compile_options(${COMPONENT_LIB} PRIVATE -DTEST_SEC=6)
#target_compile_options(${COMPONENT_LIB} PRIVATE \${DEFS})


