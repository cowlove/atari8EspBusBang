// csim: a host HTTP server behind the esp_http_server.h subset in
// csim/a800/include, so the Sidecar's pages (sidecar/main/sidecarWeb.cpp)
// can be browsed in csim. POSIX sockets, non-blocking, polled from the
// Sidecar's service loop (csimHttpdPoll()): one connection at a time, one
// request per connection, handlers run in the Sidecar's coroutine.

#include "esp_http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

struct Server {
    int listenFd = -1;
    std::vector<httpd_uri_t> handlers;
};
Server server;

struct Conn {
    int fd = -1;
    std::string in;          // the request as read so far
    size_t headerEnd = 0;    // past the blank line, once seen
    size_t contentLen = 0;
    size_t bodyRead = 0;     // httpd_req_recv()'s position in the body
    double startSec = 0;
    // The response being built.
    std::string status = "200 OK";
    std::string type = "text/html";
    std::string headers;
    bool chunked = false;    // headers sent, chunks follow
    bool sent = false;
};
Conn conn;

double nowSec()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void sendAll(int fd, const char *data, size_t len)
{
    while (len > 0) {
        const ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n > 0) {
            data += n;
            len -= n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            usleep(1000);   // the client's window is full: a small page, wait it out
        } else {
            return;   // the client went away
        }
    }
}

void closeConn()
{
    if (conn.fd >= 0) {
        close(conn.fd);
    }
    conn = Conn{};
}

std::string headerBlock(Conn &c, const char *lengthOrChunked)
{
    std::string h = "HTTP/1.1 " + c.status + "\r\nContent-Type: " + c.type + "\r\n" + c.headers +
                    lengthOrChunked + "Connection: close\r\n\r\n";
    return h;
}

Conn &connOf(httpd_req_t *r)
{
    return *static_cast<Conn *>(r->aux);
}

size_t headerValue(const std::string &head, const char *name)
{
    // Case-insensitive search for "\r\n<name>:"; returns the value's offset or npos.
    const std::string key = std::string("\r\n") + name + ":";
    for (size_t at = 0; at + key.size() <= head.size(); ++at) {
        if (strncasecmp(head.c_str() + at, key.c_str(), key.size()) == 0) {
            return at + key.size();
        }
    }
    return std::string::npos;
}

void dispatch()
{
    const std::string &in = conn.in;
    const size_t lineEnd = in.find("\r\n");
    char method[16] = {}, target[HTTPD_MAX_URI_LEN + 1] = {};
    if (sscanf(in.substr(0, lineEnd).c_str(), "%15s %512s", method, target) != 2) {
        sendAll(conn.fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", 47);
        return;
    }
    httpd_req_t req = {};
    req.handle = &server;
    req.method = !strcmp(method, "GET")    ? HTTP_GET
                 : !strcmp(method, "POST") ? HTTP_POST
                 : !strcmp(method, "PUT")  ? HTTP_PUT
                 : !strcmp(method, "HEAD") ? HTTP_HEAD
                 : !strcmp(method, "DELETE") ? HTTP_DELETE : -1;
    snprintf(req.uri, sizeof(req.uri), "%s", target);
    req.content_len = conn.contentLen;
    req.aux = &conn;
    const std::string path = std::string(target).substr(0, std::string(target).find('?'));
    bool pathKnown = false;
    for (const httpd_uri_t &h : server.handlers) {
        if (path != h.uri) {
            continue;   // exact match, as IDF's default
        }
        pathKnown = true;
        if (h.method != req.method) {
            continue;
        }
        req.user_ctx = h.user_ctx;
        if (h.handler(&req) != ESP_OK && !conn.sent && !conn.chunked) {
            httpd_resp_send_err(&req, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);
        }
        if (conn.chunked) {
            sendAll(conn.fd, "0\r\n\r\n", 5);   // a handler that didn't end its chunks
        }
        return;
    }
    httpd_resp_send_err(&req, pathKnown ? HTTPD_405_METHOD_NOT_ALLOWED : HTTPD_404_NOT_FOUND, nullptr);
}

}  // namespace

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    if (server.listenFd >= 0) {
        *handle = &server;
        return ESP_OK;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return ESP_FAIL;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config->server_port);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        printf("csim httpd: port %u: %s\n", (unsigned)config->server_port, strerror(errno));
        close(fd);
        return ESP_FAIL;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    server.listenFd = fd;
    *handle = &server;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *uri_handler)
{
    server.handlers.push_back(*uri_handler);
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    connOf(r).status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    connOf(r).type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    connOf(r).headers += std::string(field) + ": " + value + "\r\n";
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    Conn &c = connOf(r);
    const size_t len = buf_len == HTTPD_RESP_USE_STRLEN ? (buf ? strlen(buf) : 0) : (size_t)buf_len;
    const std::string head = headerBlock(c, ("Content-Length: " + std::to_string(len) + "\r\n").c_str());
    sendAll(c.fd, head.data(), head.size());
    if (r->method != HTTP_HEAD && len) {
        sendAll(c.fd, buf, len);
    }
    c.sent = true;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    Conn &c = connOf(r);
    const size_t len = buf_len == HTTPD_RESP_USE_STRLEN ? (buf ? strlen(buf) : 0) : (size_t)buf_len;
    if (!c.chunked) {
        const std::string head = headerBlock(c, "Transfer-Encoding: chunked\r\n");
        sendAll(c.fd, head.data(), head.size());
        c.chunked = true;
    }
    char size[16];
    snprintf(size, sizeof(size), "%zx\r\n", len);
    sendAll(c.fd, size, strlen(size));
    if (len) {
        sendAll(c.fd, buf, len);
    }
    sendAll(c.fd, "\r\n", 2);
    if (len == 0) {
        c.chunked = false;   // done
        c.sent = true;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str)
{
    return httpd_resp_send(r, str, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    static const char *const status[] = {"400 Bad Request", "404 Not Found", "405 Method Not Allowed",
                                         "500 Internal Server Error"};
    httpd_resp_set_status(req, status[error]);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg ? msg : status[error], HTTPD_RESP_USE_STRLEN);
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    Conn &c = connOf(r);
    const size_t left = c.contentLen - c.bodyRead;
    const size_t n = buf_len < left ? buf_len : left;
    memcpy(buf, c.in.data() + c.headerEnd + c.bodyRead, n);
    c.bodyRead += n;
    return (int)n;
}

size_t httpd_req_get_url_query_len(httpd_req_t *r)
{
    const char *q = strchr(r->uri, '?');
    return q ? strlen(q + 1) : 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    const char *q = strchr(r->uri, '?');
    if (!q) {
        return ESP_FAIL;
    }
    snprintf(buf, buf_len, "%s", q + 1);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    const size_t keyLen = strlen(key);
    for (const char *at = qry; at && *at;) {
        const char *end = strchr(at, '&');
        const size_t len = end ? (size_t)(end - at) : strlen(at);
        if (len > keyLen && !strncmp(at, key, keyLen) && at[keyLen] == '=') {
            const size_t vlen = len - keyLen - 1;
            snprintf(val, val_size, "%.*s", (int)vlen, at + keyLen + 1);
            return vlen < val_size ? ESP_OK : ESP_FAIL;   // IDF: ESP_ERR_HTTPD_RESULT_TRUNC
        }
        at = end ? end + 1 : nullptr;
    }
    return ESP_FAIL;   // IDF: ESP_ERR_NOT_FOUND
}

void csimHttpdPoll()
{
    if (server.listenFd < 0) {
        return;
    }
    // The service loop passes are frequent: look at the sockets about every millisecond.
    static double next = 0;
    const double now = nowSec();
    if (now < next) {
        return;
    }
    next = now + 0.001;
    if (conn.fd < 0) {
        const int fd = accept(server.listenFd, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        conn.fd = fd;
        conn.startSec = now;
    }
    char buf[4096];
    for (;;) {
        const ssize_t n = recv(conn.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            conn.in.append(buf, n);
            if (conn.in.size() > 64 * 1024) {
                closeConn();   // too big for these pages
                return;
            }
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            closeConn();   // closed before a whole request
            return;
        }
        break;
    }
    if (!conn.headerEnd) {
        const size_t end = conn.in.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (now - conn.startSec > 5) {
                closeConn();   // a client that never finished its request
            }
            return;
        }
        conn.headerEnd = end + 4;
        const std::string head = conn.in.substr(0, end);
        const size_t at = headerValue(head, "Content-Length");
        conn.contentLen = at == std::string::npos ? 0 : strtoul(head.c_str() + at, nullptr, 10);
    }
    if (conn.in.size() < conn.headerEnd + conn.contentLen) {
        if (now - conn.startSec > 5) {
            closeConn();
        }
        return;
    }
    dispatch();
    closeConn();
}
