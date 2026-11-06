# The Sidecar link: i2slink (hands-free I2S DMA link) and the Sidecar
# command/response protocol, shared by the host app (../../main) and the
# Sidecar app (../../sidecar).

set(priv_requires esp_driver_i2s esp_driver_gpio esp_timer esp_rom efuse esp_pm)
# i2s_dma_ring.c and i2slink.c reach into driver-private headers: the I2S
# channel internals and the GDMA pair, whose private header moved in IDF 6.
set(private_includes "${IDF_PATH}/components/esp_driver_i2s")
if(EXISTS "${IDF_PATH}/components/esp_driver_dma/src/gdma_priv.h")
    list(APPEND priv_requires esp_driver_dma esp_hal_dma)
    list(APPEND private_includes "${IDF_PATH}/components/esp_driver_dma/src")
else()
    list(APPEND private_includes "${IDF_PATH}/components/esp_hw_support/dma")
endif()

idf_component_register(
    SRCS "i2slink.c" "i2s_dma_ring.c" "sidecar.cpp"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS ${private_includes}
    PRIV_REQUIRES ${priv_requires}
)

# The link orders its stores with explicit memw; volatile serialization
# would only add more.
target_compile_options(${COMPONENT_LIB} PRIVATE -mno-serialize-volatile)
