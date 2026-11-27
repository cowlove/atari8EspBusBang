# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Main never TX-frame-auto-syncs (the Sidecar does; main only reports its
# lock): leave that code out of IRAM (components/i2slink/src/i2slink.c).
idf_build_set_property(COMPILE_DEFINITIONS "I2SLINK_TX_AUTO_SYNC_CODE=0" APPEND)
# SMB disk images follow main's Kconfig A8BB_WIFI (components/disk_image/
# smbNetwork.h): without WiFi, libsmb2 and lwIP drop out of the link. The
# Sidecar app always has them.
idf_build_set_property(COMPILE_DEFINITIONS "DISK_IMAGE_SMB_KCONFIG=1" APPEND)
# The Sidecar: CMake options, not Kconfig, so a branch's sdkconfig needn't
# differ by whether its boards have one. boot.sh -sc passes -DSIDECAR=ON;
# tools/clear_build_defines.sh turns these off for any run that doesn't pass
# them. Global definitions, always 0 or 1: components/a8log's header reads
# A8BB_LOG_VIA_SIDECAR too.
option(SIDECAR "Main uses the Sidecar coprocessor over the i2slink (A8BB_SIDECAR); turns off the status LED" OFF)
option(SIDECAR_LOOPBACK "Test mode, implies SIDECAR: the link receives its own transmissions; runs on a bare board, LOG() stays local" OFF)
option(SIDECAR_LOG_ONLY "With SIDECAR: LOG() lines go only to the Sidecar's console, not also main's (A8BB_LOG_CONSOLE_TOO 0)" OFF)
set(a8bb_sidecar 0)
set(a8bb_loopback 0)
set(a8bb_log_via_sidecar 0)
set(a8bb_log_console_too 1)
if(SIDECAR OR SIDECAR_LOOPBACK)
    set(a8bb_sidecar 1)
endif()
if(SIDECAR_LOOPBACK)
    set(a8bb_loopback 1)
elseif(a8bb_sidecar)
    set(a8bb_log_via_sidecar 1)
    if(SIDECAR_LOG_ONLY)
        set(a8bb_log_console_too 0)
    endif()
endif()
idf_build_set_property(COMPILE_DEFINITIONS "A8BB_SIDECAR=${a8bb_sidecar}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "A8BB_SIDECAR_LOOPBACK=${a8bb_loopback}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "A8BB_LOG_VIA_SIDECAR=${a8bb_log_via_sidecar}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "A8BB_LOG_CONSOLE_TOO=${a8bb_log_console_too}" APPEND)
# Core 1's bus loop last in IRAM, unpadded (main/core1_last.ld). A build
# link option, not target_linker_script(): ld resolves the script's
# INSERT AFTER .iram0.text only when the script comes before sections.ld,
# and build link options precede IDF's -T list.
idf_build_set_property(LINK_OPTIONS "-T${CMAKE_CURRENT_LIST_DIR}/main/core1_last.ld" APPEND)
project(atari8EspBusBang)
set_property(TARGET ${CMAKE_PROJECT_NAME}.elf APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/main/core1_last.ld")

# With explicit Core0 hotspot placement, the link's polling loops go in the
# core0_hotspots section, which main/link.lf maps (components/i2slink/src/i2slink_hot.h).
if(CONFIG_A8BB_CORE0_HOTSPOTS_EXPLICIT_PLACEMENT)
    idf_component_get_property(i2slink_lib i2slink COMPONENT_LIB)
    target_compile_definitions(${i2slink_lib} PRIVATE I2SLINK_HOT_SECTION=".text.core0_hotspots")
endif()

find_package(PythonInterp REQUIRED)
add_custom_command(
    TARGET ${PROJECT_NAME}.elf
    POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/tools/verify_iram_hotspots.py"
        --nm "${CMAKE_NM}"
        --elf "$<TARGET_FILE:${PROJECT_NAME}.elf>"
    COMMENT "Verifying Core0/Core1 IRAM hotspot separation"
    VERBATIM
)
# Core 1's hot data must not share an SRAM bank with IRAM code or core 0's
# interrupt stack (main/mmu.h): a layout shift that put it there broke .5.
add_custom_command(
    TARGET ${PROJECT_NAME}.elf
    POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/tools/verify_core1_hot_data.py"
        --nm "${CMAKE_NM}"
        --elf "$<TARGET_FILE:${PROJECT_NAME}.elf>"
    COMMENT "Verifying core 1 hot data stays out of IRAM code's SRAM banks"
    VERBATIM
)
add_custom_command(
    TARGET ${PROJECT_NAME}.elf
    POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/tools/verify_core0_hot_calls.py"
        --nm "${CMAKE_NM}"
        --objdump "${CMAKE_OBJDUMP}"
        --elf "$<TARGET_FILE:${PROJECT_NAME}.elf>"
        --map "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map"
    COMMENT "Verifying the Sidecar call chain stays in vetted IRAM code with no peripheral access"
    VERBATIM
)
# LOG()'s Sidecar route (main/a8log.cpp): LOGHOT() and its synchronous
# send run with core 1 hot. LOGHOT()'s formatter
# (nanoprintf, a8logFmt.cpp) writes through its own putc callbacks: a
# closed object. Skipped without A8BB_LOG_VIA_SIDECAR (its roots aren't linked).
add_custom_command(
    TARGET ${PROJECT_NAME}.elf
    POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/tools/verify_core0_hot_calls.py"
        --nm "${CMAKE_NM}"
        --objdump "${CMAKE_OBJDUMP}"
        --elf "$<TARGET_FILE:${PROJECT_NAME}.elf>"
        --map "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map"
        --root a8log_hot --root SidecarHost::logFlush
        --allow-archive libmain.a --allow-archive libsidecar_link.a --allow-archive libi2slink.a
        --closed-object a8logFmt.cpp.obj
        --check-data --what "LOGHOT()"
    COMMENT "Verifying LOGHOT() is hot-safe"
    VERBATIM
)
# The PBI request's hot part (main/pbi.cpp's handlePbiRequestHot(), before
# handlePbiRequestCold() detaches the bus): the Sidecar's PBI commands and SIO
# (mapped-buffer SIO through mmuMappedRun()), the idle disk swap, and the
# Sidecar sampler and system flags. main's code is walked too, so a header
# inline GCC emits out of line in flash is caught. Skipped without the
# Sidecar (sidecarHost's roots aren't linked).
add_custom_command(
    TARGET ${PROJECT_NAME}.elf
    POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/tools/verify_core0_hot_calls.py"
        --nm "${CMAKE_NM}"
        --objdump "${CMAKE_OBJDUMP}"
        --elf "$<TARGET_FILE:${PROJECT_NAME}.elf>"
        --map "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map"
        --root SidecarHost::checkPbiRequestHook --root SidecarHost::handlePbiRequest
        --root SidecarHost::rotateDrives --root rotateDrives
        --root sidecarSampling --root sidecarSystemFlags --root i2slink_hint
        --allow-archive libmain.a --allow-archive libsidecar_link.a --allow-archive libi2slink.a
        --closed-object a8logFmt.cpp.obj
        --check-data --what "the PBI request's Sidecar path"
    COMMENT "Verifying the PBI request's Sidecar path is hot-safe"
    VERBATIM
)
# core0Loop() and everything it reaches: core 0's whole hot regime, from
# the sled's flood going live (sledRun(), RTC fast memory) to the loop's
# exit. Two cold calls are declared: handlePbiRequestCold() and yield()'s
# vTaskDelay(), both on the detached side of ScopedInterruptEnable
# (main/pbi.cpp), and core1Stalled() (core 1 off the bus). Exits go
# through core0_exitHot() (IRAM), never the flash core0_exit().
# FAKE_SIO on the FAKE_CLOCK bench: its bank ops reprogram the fake bus's
# pins from core 0 with core 1 hot, by design (no 6502 to crash; main/
# fakeSio.cpp); only its BENCH_PINS functions are excused, nothing else.
set(FAKE_SIO_BENCH_PINS_OK "")
if(FAKE_CLOCK AND FAKE_SIO)
    foreach(f ioMux outSel holdRwHigh readDataLines probeDataLines)
        list(APPEND FAKE_SIO_BENCH_PINS_OK --peripheral-ok ${f})
    endforeach()
endif()
add_custom_command(
    TARGET ${PROJECT_NAME}.elf
    POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/tools/verify_core0_hot_calls.py"
        --nm "${CMAKE_NM}"
        --objdump "${CMAKE_OBJDUMP}"
        --elf "$<TARGET_FILE:${PROJECT_NAME}.elf>"
        --map "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map"
        --root core0Loop
        --allow-archive libmain.a --allow-archive libsidecar_link.a --allow-archive libi2slink.a
        --closed-object a8logFmt.cpp.obj
        --cold-call "handlePbiRequestHot>handlePbiRequestCold"
        --cold-call "handlePbiRequestHot>vTaskDelay"
        --cold-call "core0Loop>core1Stalled"
        ${FAKE_SIO_BENCH_PINS_OK}
        --check-data --what "core0Loop()"
    COMMENT "Verifying core0Loop() (core 0's hot regime) is hot-safe"
    VERBATIM
)
# The system monitor runs hot (main/sysMonitor.h): no flash code or data, no
# PSRAM, no peripherals. Skipped when CONFIG_A8BB_SYSMONITOR is off.
add_custom_command(
    TARGET ${PROJECT_NAME}.elf
    POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/tools/verify_core0_hot_calls.py"
        --nm "${CMAKE_NM}"
        --objdump "${CMAKE_OBJDUMP}"
        --elf "$<TARGET_FILE:${PROJECT_NAME}.elf>"
        --map "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map"
        --root SysMonitor::pbi --allow-archive libmain.a
        --closed-object sysMonitor.cpp.obj --check-data --what "the system monitor"
    COMMENT "Verifying the system monitor is hot-safe"
    VERBATIM
)
add_custom_command(
    TARGET ${PROJECT_NAME}.elf
    POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/tools/core1_cycle_count.py"
        --nm "${CMAKE_NM}"
        --objdump "${CMAKE_OBJDUMP}"
        --elf "$<TARGET_FILE:${PROJECT_NAME}.elf>"
        --refresh-bit "${CONFIG_A8BB_REFRESH_GPIO}"
        --strict
    COMMENT "Core 1 static cycle counts per sample point (P1-P6); no l32r before P1"
    VERBATIM
)

#idf_build_set_property(COMPILE_OPTIONS "-mabi=call0" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-mno-serialize-volatile" APPEND)
