#pragma once
// csim: the subset of ESP-IDF's HTTP server API (esp_http_server.h) the
// Sidecar's pages use (sidecar/main/sidecarWeb.cpp), served for real on a
// host TCP port by csim/a800/csim_httpd.cpp. One request per connection
// (Connection: close), handled from the Sidecar's service loop
// (csimHttpdPoll()), so handlers run single-threaded with the rest of the
// Sidecar app. Add calls here as pages need them, with the IDF signatures.
#include "csim.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef void *httpd_handle_t;

typedef enum {   // http_parser's numbering, as IDF has it
    HTTP_DELETE = 0,
    HTTP_GET = 1,
    HTTP_HEAD = 2,
    HTTP_POST = 3,
    HTTP_PUT = 4,
} httpd_method_t;

typedef enum {
    HTTPD_400_BAD_REQUEST,
    HTTPD_404_NOT_FOUND,
    HTTPD_405_METHOD_NOT_ALLOWED,
    HTTPD_500_INTERNAL_SERVER_ERROR,
} httpd_err_code_t;

#define HTTPD_MAX_URI_LEN 512
#define HTTPD_RESP_USE_STRLEN (-1)

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    char uri[HTTPD_MAX_URI_LEN + 1];   // path and query, as received
    size_t content_len;
    void *user_ctx;
    void *aux;   // the connection (csim_httpd.cpp)
} httpd_req_t;

typedef struct httpd_uri {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    uint16_t server_port;
    bool lru_purge_enable;
    uint16_t max_uri_handlers;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() (httpd_config_t{80, false, 8})

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);

// csim only: accept and serve what's waiting (non-blocking); from the
// Sidecar's service loop.
void csimHttpdPoll();
