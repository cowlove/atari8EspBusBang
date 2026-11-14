# The Sidecar's serial flasher for the main board: a separate, single-purpose
# app for the Sidecar's ESP32, built and flashed by hand, never from a
# script (boot.sh never touches it). It resets the main board into ROM
# download mode, writes the images staged in the Sidecar's spiffs
# (/spiffs/main/: bootloader, partition table, OTA data, app), verifies
# them and resets main into its firmware, then relays main's UART0 to this
# console. For initializing new boards, with the main board disconnected
# from the Atari (like an ESP-Prog on J5); routine updates are the Sidecar
# app's OTA. See main/main.cpp.
#
#   . tools/export_build_env.sh
#   MAIN_BUILD=./build sidecar/flash_spiffs.sh   # stage main's images (if not already there)
#   idf.py -C sidecar_serial_flasher build flash monitor
#   ...then put the Sidecar app back: idf.py -C sidecar flash
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sidecar_serial_flasher)
