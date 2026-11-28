set(MAIN_PRIV_REQUIRES
    spi_flash esp_driver_gpio esp_partition esp_timer
    hal esp_driver_ledc esp_driver_uart esp_driver_usb_serial_jtag
    esp_driver_rmt driver
    esp_mm esp_driver_dma
    spiffs esp_rom
    espcoredump esp_app_format   # the Sidecar boot report (core dump token, app SHA)
    app_update                   # OTA from the Sidecar (esp_ota_ops)
    etl
    # Main's own WiFi and SMB (A8BB_WIFI; the web server, telnet and mDNS
    # are the Sidecar's only).
    esp_wifi nvs_flash
    libsmb2
    # The Sidecar link and protocol (components/sidecar_link), shared with
    # the Sidecar app in sidecar/. Always built (IDF expands requirements
    # before sdkconfig is loaded); host use is gated on A8BB_SIDECAR (-DSIDECAR=ON),
    # and the linker drops it when unused. Also supplies atariStructs.h.
    sidecar_link
    # Disk images (components/disk_image), shared with the Sidecar app.
    disk_image
    # The bus-pin burst capture (components/bus_profiler), a component so
    # the Sidecar app can share it.
    bus_profiler
    # LOG() (components/a8log), shared with the Sidecar app.
    a8log
)

set(MAIN_SRCS
    "main.cpp" "profile.cpp" "mmu.cpp" "bmon.cpp" "pbi.cpp" "cio.cpp"
    "extMem.cpp" "arduinoLite.cpp" "core1.cpp" "wifi.cpp" "fakeBus.cpp"
    "pbirom.h" "telnetScreen.cpp" "cartridge.cpp"
    "sysMonitor.cpp" "sysMonitorCold.cpp" "sysConfig.cpp" "configText.cpp" "apt.cpp" "screenScrape.cpp" "screenshot.cpp" "sidecarHost.cpp" "hotData.cpp" "fakeSio.cpp" "a8log.cpp" "a8logFmt.cpp"
)

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
# The main board's pin map (main/pinDefs.h): version 1 (the default), the
# test pool's boards (A0-A15 on GPIO1-16, R/W on GPIO17), or 2, the
# re-spun boards (R/W on GPIO15, A14-A15 on GPIO16-17), whose core 1 decode
# is shorter (core1.cpp). Main's sources see A8BB_BOARD_VERSION.
set(BOARD_VERSION "1" CACHE STRING "Main board version (pin map): 1 or 2")
set_property(CACHE BOARD_VERSION PROPERTY STRINGS 1 2)
option(FAKE_CLOCK "Generate synthetic bus signals for standalone testing" OFF)
option(NO_REFRESH "No refresh_ input: main keeps GPIO19 and its USB console in Sidecar builds (bench pair on two USB cables; clear with cmake -U NO_REFRESH)" OFF)
option(FAKE_SIO "With FAKE_CLOCK: the SIO stress test, core 0 playing the 6502's PBI side (main/fakeSio.h)" OFF)

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
    if(SIDECAR OR SIDECAR_LOOPBACK)
        message(FATAL_ERROR "PROFILE_MODE=PIN drives GPIO48, which SIDECAR keeps "
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
foreach(define_name BOOT_CONFIG CART_IMAGE HW_TYPE RUN_SEC BOOT_TOKEN TEST_PANIC TEST_FAIL_EXIT IRQ_JAM_TEST SLED_LOOP SLED_CAPTURE BOOT_BUS_PROBE SLED_LOOP_RESTART SLED_RESTART_SYS SLED_PARK_JMP RESTART_PAD_HOLD RESET_AFTER RESET_REPEAT SIO_MOUNT_ON_USE SIO_TRACE NO_PBI_INTERRUPT HOT_CACHE_TRAP SIO_FIRST_STALL_MS SIO_FIRST_TEST COLD_CORE1_STALL TEST_CORE1_FAULT TEST_CORE1_HANG TEST_CORE0_HANG BOOT_SDX BUS_ANALYZER ROM_BOOT_PROBE
        HEAD_NOPS RD_DATA_NOPS WR_SAMPLE_NOPS
        FAKE_BUS_TEST_ADDRESS FAKE_SIO_SEED FAKE_SIO_GAP_MS FAKE_SIO_SEQ NO_LINK_POLL LINK_TEST BMON_DELAY_LOG)
    if(DEFINED ${define_name})
        list(APPEND A8BB_COMPILE_OPTIONS "-D${define_name}=${${define_name}}")
    endif()
endforeach()
if(BOARD_VERSION STREQUAL "1" OR BOARD_VERSION STREQUAL "2")
    list(APPEND A8BB_COMPILE_OPTIONS "-DA8BB_BOARD_VERSION=${BOARD_VERSION}")
else()
    message(FATAL_ERROR "BOARD_VERSION=${BOARD_VERSION}: 1 (the default) or 2")
endif()
if(FAKE_CLOCK)
    list(APPEND A8BB_COMPILE_OPTIONS "-DFAKE_CLOCK")
endif()
if(NO_REFRESH)
    list(APPEND A8BB_COMPILE_OPTIONS "-DNO_REFRESH")
endif()
if(FAKE_SIO)
    if(NOT FAKE_CLOCK)
        message(FATAL_ERROR "FAKE_SIO needs FAKE_CLOCK: on a real Atari the 6502 makes the PBI requests")
    endif()
    list(APPEND A8BB_COMPILE_OPTIONS "-DFAKE_SIO")
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE ${A8BB_COMPILE_OPTIONS})
# IDF's private heap header (registered_heaps), for logHeapInfo() in main.cpp:
# heap_caps_print_heap_info() through LOG().
idf_component_get_property(a8bb_heap_dir heap COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${a8bb_heap_dir}")
target_compile_options(${COMPONENT_LIB} PRIVATE -ffunction-sections)
