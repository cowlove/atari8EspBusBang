// The Sidecar's telnet server: see sidecarTelnet.h. Moved here from main's
// telnet.cpp; the screen half stayed on main (main/telnetScreen.cpp).
//
// Threads: the RFC 2217 server's task runs the callbacks (stream producer:
// open(), write(), close()); this file's task drains what main sent
// (stream consumer: read()); the Sidecar's service task pumps the stream
// over the link (sidecarApp.cpp).

#include "sidecarTelnet.h"

#include "a8log.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rfc2217_server.h"
#include "sidecar.h"

namespace {

rfc2217_server_t server;
SidecarStream *stream = nullptr;
volatile bool clientConnected = false;
bool discardNextInputByte = false;   // the first byte after a connect isn't the client's typing (as main had it)
uint32_t connects = 0, bytesIn = 0, bytesOut = 0;

void onConnected(void *)
{
    if (clientConnected)
        return;
    clientConnected = true;
    discardNextInputByte = true;
    ++connects;
    LOG("Sidecar telnet: client connected\n");
    stream->open();
}

void onDisconnected(void *)
{
    clientConnected = false;
    LOG("Sidecar telnet: client disconnected\n");
    stream->close();
}

void onData(void *, const uint8_t *data, size_t len)
{
    if (!clientConnected)
        onConnected(nullptr);   // on_connected isn't always called: spoof it here (as main did)
    if (discardNextInputByte && len > 0) {
        discardNextInputByte = false;
        ++data;
        --len;
    }
    bytesIn += len;
    while (len > 0) {
        const uint32_t n = stream->write(data, len);   // full: wait for the service loop to send some
        data += n;
        len -= n;
        if (len > 0)
            vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// What main sends: to the client, or the console without one. And a
// session main or the link closed while the client is still here (main
// rebooted): open a new one, at most once a second.
void telnetTask(void *)
{
    uint32_t seenCloses = stream->closes();
    TickType_t lastReopen = 0;
    uint8_t buf[256];
    for (;;) {
        if (const uint32_t n = stream->read(buf, sizeof(buf))) {
            bytesOut += n;
            if (clientConnected) {
                rfc2217_server_send_data(server, buf, n);
            } else {
                fwrite(buf, 1, n, stdout);
                fflush(stdout);
            }
            continue;
        }
        if (stream->closes() != seenCloses) {
            seenCloses = stream->closes();
            if (clientConnected && !stream->isOpen() && xTaskGetTickCount() - lastReopen > pdMS_TO_TICKS(1000)) {
                lastReopen = xTaskGetTickCount();
                stream->open();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

void sidecarTelnetStart(SidecarStream &s, uint16_t port)
{
    if (stream)
        return;
    stream = &s;
    rfc2217_server_config_t config = {
        .ctx = nullptr,
        .on_client_connected = onConnected,
        .on_client_disconnected = onDisconnected,
        .on_baudrate = nullptr,
        .on_control = nullptr,
        .on_purge = nullptr,
        .on_data_received = onData,
        .port = port,
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_core_id = 0,
    };
    if (rfc2217_server_create(&config, &server) != ESP_OK || rfc2217_server_start(server) != ESP_OK) {
        LOG("Sidecar telnet: server failed to start\n");
        return;
    }
    xTaskCreatePinnedToCore(telnetTask, "telnet", 3072, nullptr, tskIDLE_PRIORITY + 5, nullptr, 0);
    LOG("Sidecar telnet: server on port %u\n", (unsigned)port);
}

bool sidecarTelnetRunning()
{
    return stream != nullptr;
}

void sidecarTelnetReport()
{
    printf("Sidecar telnet: %s, client %s, connects %u, bytes in %u out %u\n",
           stream ? "running" : "not started", clientConnected ? "connected" : "none", (unsigned)connects,
           (unsigned)bytesIn, (unsigned)bytesOut);
}
