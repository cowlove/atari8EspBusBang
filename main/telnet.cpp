#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
//#include "protocol_examples_common.h"

#include "rfc2217_server.h"
#include "main.h"
#include "util.h"
#include "mmu.h"

static const char *TAG = "app_main";
static rfc2217_server_t s_server;
static SemaphoreHandle_t s_client_connected;
static SemaphoreHandle_t s_client_disconnected;

static void on_connected(void *ctx);
static void on_disconnected(void *ctx);
static void on_data_received(void *ctx, const uint8_t *data, size_t len);
static volatile bool connected = false;


volatile int oldCursor = 0;

static void send(const char *msg) { 
    rfc2217_server_send_data(s_server, (const uint8_t *) msg, strlen(msg));
}

static void updateScreen() { 
    static uint8_t textBeforeCursor[40] = {0};
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    uint8_t *mem = mmuCheckRangeMapped(savmsc, 24 * 40);
    if (mem == NULL) {
        send("\r\nSAVMSC screen memory not mapped\r\n");
        return;
    }

    int newCursor = atariRam[84] * 40 + atariRam[85];

    // see if the text before the cursor has moved up the screen, 
    // update oldCursor if it has 
    for(int newOc = oldCursor; newOc > 0; newOc -= 40) {
        bool match = true;
        for(int n = 0; n < sizeof(textBeforeCursor); n++) {
            int spos = newOc - sizeof(textBeforeCursor) + n - 1;
            if (spos >= 0 && mem[spos] != textBeforeCursor[n]) {
                match = false;
                break;
            }
        }
        if (match == true) {
            oldCursor = newOc;
            break;
        }
        // TODO: gets tripped too much, maybe the 6502 is in the middle of scrolling?
        // maybe looks for a way to avoid sampling in-motion screen memory?  Maybe avoid 
        //if (newOc <= 40)
        //   oldCursor = 0;  
    }

    // Send all the characters between oldCursor and newCursor, inserting
    // line breaks at row ends
    for(int n = oldCursor; n < newCursor; n++) { 
        if (n % 40 == 0) {
            send("\r\n");
        } else if (n % 40 > 1) {
            char buf[16];
            screenMemToAscii(buf, sizeof(buf), mem[n]);
            send(buf);
        }
    }
    oldCursor = newCursor;
    // copy the new text before the cursor to detect scrolling in next invocation
    for(int n = 0; n < sizeof(textBeforeCursor); n++) {
        int spos = oldCursor - sizeof(textBeforeCursor) + n - 1;
        textBeforeCursor[n] = (spos >= 0) ? mem[spos] : 0;
    }
}

void telnetServerRun() { 
    if (xSemaphoreTake(s_client_connected, 1) == pdTRUE) { 
        printf("client connected\n");
        oldCursor = 0;
        ESP_LOGI(TAG, "Client connected, sending greeting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        const char *msg = "\r\nCONNECTED!\r\n";
        //rfc2217_server_send_data(s_server, (const uint8_t *) msg, strlen(msg));
        //updateScreen();
    }
    if (xSemaphoreTake(s_client_disconnected, 1) == pdTRUE) { 
        ESP_LOGI(TAG, "Client disconnected");
    }
    if (connected)
        updateScreen();
}
void startTelnetServer(void)
{
    s_client_connected = xSemaphoreCreateBinary();
    s_client_disconnected = xSemaphoreCreateBinary();

    rfc2217_server_config_t config = {
        .ctx = NULL,
        .on_client_connected = on_connected,
        .on_client_disconnected = on_disconnected,
        .on_baudrate = NULL,
        .on_control = NULL,
        .on_purge = NULL,
        .on_data_received = on_data_received,
        .port = 3333,
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_core_id = 0
    };

    ESP_ERROR_CHECK(rfc2217_server_create(&config, &s_server));

    ESP_LOGI(TAG, "Starting RFC2217 server on port %u", config.port);

    ESP_ERROR_CHECK(rfc2217_server_start(s_server));
}


//??? on connected isn't being called
static void on_connected(void *ctx)
{
    xSemaphoreGive(s_client_connected);
}

static void on_disconnected(void *ctx)
{
    connected = false;
    xSemaphoreGive(s_client_disconnected);
}

static void on_data_received(void *ctx, const uint8_t *data, size_t len)
{
    if (!connected) {
        on_connected(NULL); // on_connected not getting called, spoof it here 
        connected = true;
    }

    putKeys((const char *)data, len);
    //updateScreen();
#if 0 
    uint8_t c = '-';
    ESP_LOGI(TAG, "Received %u byte(s)", (unsigned) len);
    // Echo back the received data
    rfc2217_server_send_data(s_server, &c, 1);
    rfc2217_server_send_data(s_server, data, len);
#endif
}
