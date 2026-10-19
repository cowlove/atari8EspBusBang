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

#idf_build_set_property(COMPILE_OPTIONS "-mabi=call0" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-mno-serialize-volatile" APPEND)
