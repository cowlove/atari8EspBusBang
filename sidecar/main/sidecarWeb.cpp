// The Sidecar's web pages: see sidecarWeb.h.

#include "sidecarWeb.h"

#include <cstdio>

#include "esp_timer.h"

namespace {

uint32_t requests = 0;

// "/": the start page.
esp_err_t indexHandler(httpd_req_t *req)
{
    ++requests;
    char page[512];
    snprintf(page, sizeof(page),
             "<!doctype html><html><head><meta charset=\"utf-8\"><title>Atari8 Sidecar</title></head>"
             "<body><h1>Atari8 Sidecar</h1><p>Up %lld s, %u requests.</p>"
             "<p><a href=\"/hello\">/hello</a></p></body></html>\n",
             (long long)(esp_timer_get_time() / 1000000), (unsigned)requests);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// "/hello": plain text (main's old page).
esp_err_t helloHandler(httpd_req_t *req)
{
    ++requests;
    char page[160];
    snprintf(page, sizeof(page), "Hello World! Atari8 Sidecar, up %lld s, %u requests\n",
             (long long)(esp_timer_get_time() / 1000000), (unsigned)requests);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

}  // namespace

void sidecarWebRegister(httpd_handle_t server)
{
    static const httpd_uri_t pages[] = {
        {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr},
        {.uri = "/hello", .method = HTTP_GET, .handler = helloHandler, .user_ctx = nullptr},
    };
    for (const httpd_uri_t &page : pages) {
        httpd_register_uri_handler(server, &page);
    }
}

uint32_t sidecarWebRequests()
{
    return requests;
}
