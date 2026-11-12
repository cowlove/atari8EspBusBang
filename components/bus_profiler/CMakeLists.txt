# bus_profiler: the bus-pin burst capture and its histograms, used by the
# host app (main/profile.cpp); a component so the Sidecar app can share it.
idf_component_register(
    SRCS "bus_profiler.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio hal
    PRIV_REQUIRES freertos
)
