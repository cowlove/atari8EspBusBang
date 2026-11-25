#!/bin/bash
# The boot configuration text form's host test (config_text_test.cpp).
set -e
cd "$(dirname "$0")"
mkdir -p build
g++ -std=gnu++17 -g -Wall -DCSIM -Iinclude -I../../main -I../../components/sidecar_link/include \
    config_text_test.cpp ../../main/sysConfig.cpp ../../main/configText.cpp -o build/config_text_test
exec build/config_text_test
