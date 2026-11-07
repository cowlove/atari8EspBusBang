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
    "${CMAKE_CURRENT_LIST_DIR}/../components/disk_image")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sidecar)
