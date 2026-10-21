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
#include "sysConfig.h"

static const char *TAG = "app_main";
static constexpr uint8_t telnetRefreshHotkey = 0x12;
static constexpr uint8_t telnetExitHotkey = 0x10;
static constexpr size_t telnetInputLogLineMax = 96;
static rfc2217_server_t s_server;
static SemaphoreHandle_t s_client_connected;
static SemaphoreHandle_t s_client_disconnected;
static bool discardNextInputByte = false;
static char inputLogLine[telnetInputLogLineMax + 1] = {};
static size_t inputLogLineLength = 0;
static bool inputLogSawCarriageReturn = false;

static void on_connected(void *ctx);
static void on_disconnected(void *ctx);
static void on_data_received(void *ctx, const uint8_t *data, size_t len);
static volatile bool connected = false;


volatile int oldCursor = 0;
static int previousCursorRow = -1;
static constexpr int screenRows = 24;
static constexpr int screenColumns = 40;
static constexpr int screenBytes = screenRows * screenColumns;
static constexpr int contextBytes = 40;
static uint8_t textBeforeCursor[contextBytes] = {0};
static bool forceFullScreenRefresh = false;

static void resetScreenTracking()
{
    oldCursor = 0;
    previousCursorRow = -1;
    memset(textBeforeCursor, 0, sizeof(textBeforeCursor));
    forceFullScreenRefresh = true;
}

static void resetInputLog()
{
    inputLogLineLength = 0;
    inputLogLine[0] = '\0';
    inputLogSawCarriageReturn = false;
}

static void logInputLineBytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = data[i];
        if (ch == '\n' && inputLogSawCarriageReturn) {
            inputLogSawCarriageReturn = false;
            continue;
        }

        inputLogSawCarriageReturn = false;
        if (ch == '\r' || ch == '\n') {
            inputLogLine[inputLogLineLength] = '\0';
            ESP_LOGI(TAG, "TELNET: %s", inputLogLine);
            resetInputLog();
            inputLogSawCarriageReturn = ch == '\r';
        } else if (ch == '\b' || ch == 0x7f) {
            if (inputLogLineLength > 0)
                inputLogLine[--inputLogLineLength] = '\0';
        } else if (ch >= 0x20 && ch <= 0x7e) {
            if (inputLogLineLength == telnetInputLogLineMax)
                resetInputLog();
            inputLogLine[inputLogLineLength++] = (char)ch;
            inputLogLine[inputLogLineLength] = '\0';
        }
    }
}

static void send(const char *msg) { 
    rfc2217_server_send_data(s_server, (const uint8_t *) msg, strlen(msg));
}

static void updateScreen() { 
    uint32_t savmsc = (atariRam[89] << 8) + atariRam[88];
    if (savmsc + screenBytes > UINT16_MAX + 1U) {
        oldCursor = 0;
        previousCursorRow = -1;
        return;
    }
    uint8_t *mem = mmuCheckRangeMapped(mmuStateSaved, savmsc, screenBytes);
    if (mem == NULL) {
        send("\r\nSAVMSC screen memory not mapped\r\n");
        return;
    }

    int cursorRow = atariRam[84];
    int cursorColumn = atariRam[85];
    if (cursorRow >= screenRows || cursorColumn >= screenColumns) {
        oldCursor = 0;
        previousCursorRow = -1;
        return;
    }
    int newCursor = cursorRow * screenColumns + cursorColumn;
    bool refreshingFromTop = forceFullScreenRefresh;
    bool largeUpwardCursorJump = !refreshingFromTop && previousCursorRow - cursorRow > 6;
    previousCursorRow = cursorRow;
    if (refreshingFromTop || largeUpwardCursorJump)
        oldCursor = 0;
    if (oldCursor < 0 || oldCursor >= screenBytes) {
        oldCursor = 0;
    }

    // TODO: Consider per-row CRCs and matching a shifted run of rows to recognize scrolls
    // more robustly than a single 40-byte window. Snapshot comparison cannot detect a
    // scroll that leaves both the screen contents and cursor unchanged between samples.
    // see if the text before the cursor has moved up the screen, 
    // update oldCursor if it has 
    if (!refreshingFromTop) {
        for(int newOc = oldCursor; newOc > 0; newOc -= 40) {
            bool match = true;
            for(int n = 0; n < contextBytes; n++) {
                int spos = newOc - contextBytes + n - 1;
                if (spos < 0 || spos >= screenBytes || mem[spos] != textBeforeCursor[n]) {
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
    }

    bool fullRefresh = refreshingFromTop || oldCursor == 0;
    if (fullRefresh)
        send("\r\n");

    for (int rowStart = oldCursor; rowStart < newCursor;) {
        int rowEnd = ((rowStart / screenColumns) + 1) * screenColumns;
        if (rowEnd > newCursor)
            rowEnd = newCursor;

        if (rowStart % screenColumns == 0 && !(fullRefresh && rowStart == 0))
            send("\r\n");

        int contentEnd = rowEnd;
        int rowBoundary = ((rowStart / screenColumns) + 1) * screenColumns;
        if (rowEnd == rowBoundary) {
            while (contentEnd > rowStart && mem[contentEnd - 1] == 0)
                --contentEnd;
        }

        int contentStart = rowStart;
        int firstContentColumn = ((rowStart / screenColumns) * screenColumns) + 2;
        if (contentStart < firstContentColumn)
            contentStart = firstContentColumn;

        for (int n = contentStart; n < contentEnd; ++n) {
            char buf[16];
            screenMemToAscii(buf, sizeof(buf), mem[n]);
            send(buf);
        }

        rowStart = rowEnd;
    }
    oldCursor = newCursor;
    // copy the new text before the cursor to detect scrolling in next invocation
    for(int n = 0; n < contextBytes; n++) {
        int spos = oldCursor - contextBytes + n - 1;
        textBeforeCursor[n] = (spos >= 0 && spos < screenBytes) ? mem[spos] : 0;
    }
    forceFullScreenRefresh = false;
}

void telnetServerRun() { 
    if (xSemaphoreTake(s_client_connected, 1) == pdTRUE) { 
        printf("TELNET: client connected\n");
        resetScreenTracking();
        ESP_LOGI(TAG, "Client connected, sending greeting");
        send(config.telnetServerReadonly
            ? "\r\nCONNECTED! READ ONLY\r\n"
            : "\r\nCONNECTED! READ/WRITE\r\n");
    }
    if (xSemaphoreTake(s_client_disconnected, 1) == pdTRUE) { 
        ESP_LOGI(TAG, "TELNET: client disconnected");
    }
    if (connected)
        updateScreen();
}
void startTelnetServer(void)
{
    s_client_connected = xSemaphoreCreateBinary();
    s_client_disconnected = xSemaphoreCreateBinary();

    rfc2217_server_config_t serverConfig = {
        .ctx = NULL,
        .on_client_connected = on_connected,
        .on_client_disconnected = on_disconnected,
        .on_baudrate = NULL,
        .on_control = NULL,
        .on_purge = NULL,
        .on_data_received = on_data_received,
        .port = ::config.telnetServerPort,
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_core_id = 0
    };

    ESP_ERROR_CHECK(rfc2217_server_create(&serverConfig, &s_server));

    ESP_LOGI(TAG, "Starting RFC2217 server on port %u", serverConfig.port);

    ESP_ERROR_CHECK(rfc2217_server_start(s_server));
}


//??? on connected isn't being called
static void on_connected(void *ctx)
{
    connected = true;
    discardNextInputByte = true;
    resetInputLog();
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

    size_t start = 0;
    if (discardNextInputByte && len > 0) {
        discardNextInputByte = false;
        start = 1;
    }
    logInputLineBytes(data + start, len - start);
    if (config.telnetServerReadonly)
        return;

    for (size_t i = start; i < len; ++i) {
        if (data[i] == telnetRefreshHotkey) {
            if (!config.telnetServerReadonly && i > start)
                putKeys((const char *)data + start, i - start);

            oldCursor = 0;
            start = i + 1;
            continue;
        }

        if (data[i] != telnetExitHotkey)
            continue;

        if (!config.telnetServerReadonly && i > start)
            putKeys((const char *)data + start, i - start);

        exitReason = "4 Telnet Ctrl-P exit hotkey received";
        exitFlag = 1;
        rfc2217_server_disconnect_client(s_server);
        return;
    }

    if (!config.telnetServerReadonly && start < len)
        putKeys((const char *)data + start, len - start);
    //updateScreen();
#if 0 
    uint8_t c = '-';
    ESP_LOGI(TAG, "Received %u byte(s)", (unsigned) len);
    // Echo back the received data
    rfc2217_server_send_data(s_server, &c, 1);
    rfc2217_server_send_data(s_server, data, len);
#endif
}
