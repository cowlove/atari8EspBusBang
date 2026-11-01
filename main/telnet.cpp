#ifdef CSIM
#include "csim.h"
#endif

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
#include "screenScrape.h"

static const char *TAG = "app_main";
static constexpr uint8_t telnetBreakHotkey = 0x03;
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
static constexpr int screenColumns = AtariScreenColumns;
static constexpr int screenBytes = AtariScreenBytes;
static bool forceFullScreenRefresh = false;
static AtariScreenSnapshot previousTelnetScreen = {};
static bool havePreviousTelnetScreen = false;

static void resetScreenTracking()
{
    oldCursor = 0;
    forceFullScreenRefresh = true;
    havePreviousTelnetScreen = false;
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
        if (ch == '\r' || ch == '\n' || ch == 0x9b) {
            inputLogLine[inputLogLineLength] = '\0';
            printf("TELNET: %s\n", inputLogLine);
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
    AtariScreenSnapshot screen;
    AtariScreenReadResult result = readAtariScreen(screen);
    if (result == AtariScreenReadResult::AddressOutOfRange ||
        result == AtariScreenReadResult::InvalidCursor ||
        result == AtariScreenReadResult::Unstable) {
        havePreviousTelnetScreen = false;
        return;
    }
    if (result == AtariScreenReadResult::NotMapped) {
        send("\r\nSAVMSC screen memory not mapped\r\n");
        return;
    }

    const uint8_t *mem = screen.cells;
    int cursorRow = screen.cursorRow;
    int cursorColumn = screen.cursorColumn;
    int newCursor = cursorRow * screenColumns + cursorColumn;
    bool screenChanged = !havePreviousTelnetScreen ||
                         previousTelnetScreen.address != screen.address;
    bool fullRefresh = forceFullScreenRefresh || screenChanged;
    int scrollRows = 0;
    if (!fullRefresh && havePreviousTelnetScreen) {
        const AtariScreenAlignment alignment =
            findAtariScreenAlignment(previousTelnetScreen, screen);
        if (alignment.confident)
            scrollRows = alignment.scrollRows;
    }
    if (!fullRefresh && scrollRows == 0 &&
        atariScreenNeedsFullResync(previousTelnetScreen, screen))
        fullRefresh = true;

    if (fullRefresh) {
        oldCursor = 0;
    } else if (scrollRows > 0) {
        oldCursor = oldCursor - scrollRows * screenColumns;
        if (oldCursor < 0)
            oldCursor = 0;
    } else if (newCursor < oldCursor) {
        oldCursor = newCursor;
        previousTelnetScreen = screen;
        havePreviousTelnetScreen = true;
        return;
    }

    if (oldCursor < 0 || oldCursor > screenBytes)
        oldCursor = 0;

    if (fullRefresh)
        send("\r\n");

    for (int rowStart = oldCursor; rowStart < newCursor;) {
        int rowEnd = ((rowStart / screenColumns) + 1) * screenColumns;
        if (rowEnd > newCursor)
            rowEnd = newCursor;

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

        if (rowEnd == rowBoundary)
            send("\r\n");

        rowStart = rowEnd;
    }
    oldCursor = newCursor;
    previousTelnetScreen = screen;
    havePreviousTelnetScreen = true;
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
        printf("TELNET: client disconnected\n");
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
        if (data[i] == telnetBreakHotkey) {
            if (i > start)
                putKeys((const char *)data + start, i - start);

            atariRam[0x11] = 0;
            start = i + 1;
            continue;
        }

        if (data[i] == telnetRefreshHotkey) {
            if (!config.telnetServerReadonly && i > start)
                putKeys((const char *)data + start, i - start);

            oldCursor = 0;
            forceFullScreenRefresh = true;
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
