idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES bus_profiler esp_driver_gpio esp_driver_ledc freertos
)
if(DEFINED SIDECAR_PROFILER_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        SIDECAR_PROFILER_SELFTEST=${SIDECAR_PROFILER_SELFTEST})
endif()
