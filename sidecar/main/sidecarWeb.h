#pragma once
// The Sidecar's web pages, on ESP-IDF's HTTP server API (esp_http_server.h).
// Built into the Sidecar app (served by sidecarNet.cpp's httpd, its own
// task) and into csim (served on a host port by csim/a800/csim_httpd.cpp,
// from the Sidecar's service loop), so pages are developed and tried in
// csim with the code the board runs.
//
// Threads: on the board the handlers run in the httpd task, beside the
// service loop: read shared state with care.

#include <cstdint>
#include "esp_http_server.h"

// Register the pages on a started server.
void sidecarWebRegister(httpd_handle_t server);
uint32_t sidecarWebRequests();
