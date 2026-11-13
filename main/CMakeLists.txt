set(MAIN_PRIV_REQUIRES
    spi_flash esp_driver_gpio esp_partition esp_timer
    hal esp_driver_ledc esp_driver_uart esp_driver_usb_serial_jtag
    esp_driver_rmt driver
    esp_mm esp_driver_dma
    spiffs esp_rom
    etl
    esp_wifi nvs_flash
    esp_http_server
    libsmb2
    igrr__rfc2217-server
    # The Sidecar link and protocol (components/sidecar_link), shared with
    # the Sidecar app in sidecar/. Always built (IDF expands requirements
    # before sdkconfig is loaded); host use is gated on A8BB_SIDECAR_ENABLE,
    # and the linker drops it when unused. Also supplies atariStructs.h.
    sidecar_link
    # Disk images (components/disk_image), shared with the Sidecar app.
    disk_image
    # The bus-pin burst capture (components/bus_profiler), a component so
    # the Sidecar app can share it.
    bus_profiler
)

set(MAIN_SRCS
    "main.cpp" "profile.cpp" "mmu.cpp" "bmon.cpp" "pbi.cpp" "cio.cpp"
    "extMem.cpp" "arduinoLite.cpp" "core1.cpp" "wifi.cpp" "fakeBus.cpp"
    "pbirom.h" "telnet.cpp" "cartridge.cpp"
    "sysMonitor.cpp" "sysMonitorCold.cpp" "sysConfig.cpp" "apt.cpp" "screenScrape.cpp" "sidecarHost.cpp" "hotData.cpp"
)

if(CONFIG_A8BB_ENABLE_MDNS)
    list(APPEND MAIN_PRIV_REQUIRES espressif__mdns)
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    PRIV_REQUIRES ${MAIN_PRIV_REQUIRES}

    # Use DRAM_ATTR and IRAM_ATTR instead of placing the entire project in SRAM
    # Better granularity and control frees up more SRAM
    LDFRAGMENTS "./link.lf"
)
set_source_files_properties(pbi.cpp cio.cpp profile.cpp PROPERTIES COMPILE_OPTIONS "-mtext-section-literals")
# Stop the bare-metal core 1 loop on any panic (see __wrap_esp_panic_handler).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
add_custom_command(
    OUTPUT ${COMPONENT_DIR}/pbirom.h
    COMMAND ${CMAKE_COMMAND} -E echo ${COMPONENT_DIR} "Generating header file..."
    COMMAND make -C "${COMPONENT_DIR}" pbirom.h
    DEPENDS ${COMPONENT_DIR}/pbirom.asm ${COMPONENT_DIR}/asmDefs.h
)

# gitVersion.h (gitignored): GIT_VERSION, from `git describe`, rewritten by
# tools/updateGitH.sh on every build but only when it changed, so nothing
# recompiles otherwise. A fresh checkout gets it on its first build.
idf_build_get_property(a8bb_project_dir PROJECT_DIR)
add_custom_target(a8bb_git_version
    COMMAND bash "${a8bb_project_dir}/tools/updateGitH.sh"
    BYPRODUCTS "${COMPONENT_DIR}/gitVersion.h"
    COMMENT "Updating main/gitVersion.h"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} a8bb_git_version)

target_compile_options(${COMPONENT_LIB} PRIVATE -mno-serialize-volatile)
set(A8BB_COMPILE_OPTIONS)
set(PROFILE_MODE "OFF" CACHE STRING "Profiling mode: OFF, GPIO, PIN, or LEGACY")
set_property(CACHE PROFILE_MODE PROPERTY STRINGS OFF GPIO PIN LEGACY)
set(PROFILE_POINT "" CACHE STRING "Core1 instrumentation point (0 through 6), for PIN or LEGACY")
option(FAKE_CLOCK "Generate synthetic bus signals for standalone testing" OFF)
option(FAKE_SIO "With FAKE_CLOCK: poke a D1: sector-1 read PBI request twice a second (bench smoke test)" OFF)

string(TOUPPER "${PROFILE_MODE}" PROFILE_MODE_NORMALIZED)
if(NOT PROFILE_MODE_NORMALIZED MATCHES "^(OFF|GPIO|PIN|LEGACY)$")
    message(FATAL_ERROR "PROFILE_MODE must be OFF, GPIO, PIN, or LEGACY")
endif()

if(PROFILE_MODE_NORMALIZED STREQUAL "PIN" OR PROFILE_MODE_NORMALIZED STREQUAL "LEGACY")
    if(NOT PROFILE_POINT MATCHES "^[0-6]$")
        message(FATAL_ERROR "PROFILE_POINT must be 0 through 6 for PROFILE_MODE=${PROFILE_MODE_NORMALIZED}")
    endif()
    list(APPEND A8BB_COMPILE_OPTIONS "-DPROF${PROFILE_POINT}")
endif()

# GPIO: passively sample the live bus pins (dedic_gpio inputs) for a moment
# before each PBI command; it only reads pins, so it works with the Sidecar.
# PIN: also make core 1 toggle GPIO48 at PROFILE_POINT, and sample that with
# the same routine; GPIO48 is the Sidecar link's BCLK, so not with the Sidecar.
if(PROFILE_MODE_NORMALIZED STREQUAL "GPIO" OR PROFILE_MODE_NORMALIZED STREQUAL "PIN")
    list(APPEND A8BB_COMPILE_OPTIONS "-DPROFILE_GPIO_SAMPLE")
endif()
if(PROFILE_MODE_NORMALIZED STREQUAL "PIN")
    if(CONFIG_A8BB_SIDECAR_ENABLE)
        message(FATAL_ERROR "PROFILE_MODE=PIN drives GPIO48, which A8BB_SIDECAR_ENABLE keeps "
                            "for the Sidecar link (PROFILE_MODE=GPIO only reads pins and works)")
    endif()
    list(APPEND A8BB_COMPILE_OPTIONS "-DPROFILE_GPIO_PIN_OUTPUT")
endif()

# The old relative timing knobs are gone: refuse them rather than build
# without them. Use the absolute ones in hwType.h, e.g. -DWR_SAMPLE_NOPS=10.
foreach(old_name RD_DELAY_ADJ WR_DELAY_ADJ HEAD_NOPS_ADJ RD_DATA_NOPS_ADJ)
    if(DEFINED ${old_name})
        message(FATAL_ERROR "${old_name} was replaced by absolute cycle counts in "
                            "main/hwType.h (HEAD_NOPS, RD_DATA_NOPS, WR_SAMPLE_NOPS); "
                            "pass one of those instead")
    endif()
endforeach()
# The core 1 release-placement and dedicated-GPIO experiments ended (and
# with them the release delays): the
# loop now always releases extSel_/mpd_ (core 1 dedicated outputs) after the
# clock fall and the data bus after the address read. A cached value (e.g.
# from a test-pool soak) fails here; clear it with cmake -U NAME build.
foreach(old_name RELEASE_PLACEMENT EXTSEL_DEDIC RD_RELEASE_DELAY WR_RELEASE_DELAY)
    if(DEFINED ${old_name})
        message(FATAL_ERROR "${old_name} is gone: core 1 has one release scheme now "
                            "(main/core1.cpp). Clear it: cmake -U ${old_name} <build dir>")
    endif()
endforeach()
foreach(define_name BOOT_CONFIG HW_TYPE RUN_SEC BOOT_SDX BUS_ANALYZER ROM_BOOT_PROBE
        HEAD_NOPS RD_DATA_NOPS WR_SAMPLE_NOPS
        FAKE_BUS_TEST_ADDRESS)
    if(DEFINED ${define_name})
        list(APPEND A8BB_COMPILE_OPTIONS "-D${define_name}=${${define_name}}")
    endif()
endforeach()
if(FAKE_CLOCK)
    list(APPEND A8BB_COMPILE_OPTIONS "-DFAKE_CLOCK")
endif()
if(FAKE_SIO)
    if(NOT FAKE_CLOCK)
        message(FATAL_ERROR "FAKE_SIO needs FAKE_CLOCK: on a real Atari the 6502 makes the PBI requests")
    endif()
    list(APPEND A8BB_COMPILE_OPTIONS "-DFAKE_SIO")
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE ${A8BB_COMPILE_OPTIONS})
target_compile_options(${COMPONENT_LIB} PRIVATE -ffunction-sections)
