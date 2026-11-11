# bus_profiler: the bus-pin burst capture and its histograms, shared by the
# host app (main/profile.cpp) and the Sidecar profiler (sidecar_profiler/).
idf_component_register(
    SRCS "bus_profiler.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio hal
    PRIV_REQUIRES freertos
)
