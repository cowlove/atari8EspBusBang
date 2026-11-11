# The Sidecar profiler: firmware for the Sidecar board that, instead of the
# Sidecar link, samples the Atari bus pins it is wired to and prints timing
# histograms (components/bus_profiler, shared with the host's
# PROFILE_MODE=GPIO). Unlike the host's capture, which only runs while the
# 6502 waits in the PBI driver, it samples the bus all the time: real ROM
# reads, ANTIC DMA, and core 1 while core 0 is busy. A separate IDF project,
# built with the same ESP-IDF as the host (tools/export_build_env.sh):
#
#   . tools/export_build_env.sh
#   idf.py -C sidecar_profiler build flash monitor
#   (or sidecar_profiler/run_sidecar_profiler.sh, which records the output)
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/bus_profiler")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sidecar_profiler)
