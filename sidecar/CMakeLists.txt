# The Sidecar app: firmware for the optional second ESP32 (see
# ../components/sidecar_link/sidecar.h). A separate IDF project in the same
# repo; it shares the sidecar_link and disk_image components with the host app, and
# builds with the same ESP-IDF as the host (tools/export_build_env.sh).
#
#   . tools/export_build_env.sh
#   idf.py -C sidecar build flash monitor
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/sidecar_link"
    "${CMAKE_CURRENT_LIST_DIR}/../components/disk_image"
    "${CMAKE_CURRENT_LIST_DIR}/../components/bus_profiler"   # sidecarSampler.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../components/libsmb2")   # disk_image: smb: diskSpecs

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sidecar)

# libsmb2 (components/libsmb2, a wrapper around its libsmb2/ submodule) warns about these in its
# own sources. The host app builds with CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS,
# so they were never fatal there; keep them non-fatal here without relaxing
# the rest of the Sidecar build.
idf_component_get_property(libsmb2_lib libsmb2 COMPONENT_LIB)
target_compile_options(${libsmb2_lib} PRIVATE
    -Wno-error=implicit-fallthrough
    -Wno-error=type-limits)
