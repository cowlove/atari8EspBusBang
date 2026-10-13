#!/usr/bin/env bash

if [[ -z "${IDF_PATH:-}" ]] || ! command -v idf.py >/dev/null 2>&1 || \
    ! command -v esptool.py >/dev/null 2>&1 || ! command -v python >/dev/null 2>&1 || \
    [[ ! -r "${IDF_PATH}/components/spiffs/spiffsgen.py" ]]; then
    printf 'ESP-IDF environment is not initialized in this shell.\n' >&2
    printf 'Initialize it first with: . ~/esp/esp-idf/export.sh\n' >&2
    return 1
fi
