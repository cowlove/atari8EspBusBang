# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS components/libsmb2)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(atari8EspBusBang)

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
        --strict
    COMMENT "Core 1 static cycle counts per sample point (P1-P6); no l32r before P1"
    VERBATIM
)

#idf_build_set_property(COMPILE_OPTIONS "-mabi=call0" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-mno-serialize-volatile" APPEND)
