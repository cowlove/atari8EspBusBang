# i2slink: a hands-free, bidirectional link between two ESP32-S3s over one
# I2S port's DMA on each end (BCLK and one data wire each way). See
# README.md and include/i2slink.h.

set(priv_requires esp_driver_gpio esp_driver_pcnt esp_timer esp_rom efuse esp_pm)
# The link reaches into driver-private headers: the I2S channel internals
# and the GDMA pair, whose private header moved in IDF 6.
set(private_includes "src" "${IDF_PATH}/components/esp_driver_i2s")
if(EXISTS "${IDF_PATH}/components/esp_driver_dma/src/gdma_priv.h")
    list(APPEND priv_requires esp_driver_dma esp_hal_dma)
    list(APPEND private_includes "${IDF_PATH}/components/esp_driver_dma/src")
else()
    list(APPEND private_includes "${IDF_PATH}/components/esp_hw_support/dma")
endif()

idf_component_register(
    SRCS "src/i2slink.c" "src/i2s_dma_ring.c" "src/i2slink_pins.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${private_includes}
    REQUIRES esp_driver_i2s
    PRIV_REQUIRES ${priv_requires}
)

# The link orders its stores with explicit memw; volatile serialization
# would only add more.
target_compile_options(${COMPONENT_LIB} PRIVATE -mno-serialize-volatile)
