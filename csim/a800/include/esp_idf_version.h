#pragma once
// CSIM: the ESP-IDF version the firmware builds with (sets i2slink defaults).
#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(6, 1, 0)
