// Main's half of the Sidecar's telnet server: see telnetScreen.h. The
// screen tracking (what's new since the last update, scrolling) moved here
// from main's old telnet server (now sidecar/main/sidecarTelnet.cpp).

#ifdef CSIM
#include "csim.h"
#endif

#include "telnetScreen.h"

#include "sdkconfig.h"

#if A8BB_SIDECAR
#include "a8log.h"
#include <string.h>

#include "esp_heap_caps.h"
#include "main.h"
#include "util.h"
#include "mmu.h"
#include "sysConfig.h"
#include "screenScrape.h"
#include "sidecarHost.h"

namespace {

constexpr uint8_t telnetBreakHotkey = 0x03;
constexpr uint8_t telnetRefreshHotkey = 0x12;
constexpr uint8_t telnetExitHotkey = 0x10;
constexpr size_t telnetInputLogLineMax = 96;
// Stream buffers (powers of two), from the heap so main's .bss (and core
// 1's hot data behind it) doesn't move. A full screen is ~1 KB of text: a
// full TX buffer is sent (pump()) and refilled.
constexpr uint32_t kRxBytes = 256;
constexpr uint32_t kTxBytes = 1024;
constexpr uint32_t kPumpUs = 300000;   // the Sidecar answers within one service pass

DRAM_ATTR SidecarStream telnetStream;
bool streamReady = false;
uint32_t seenOpens = 0, seenCloses = 0;

char inputLogLine[telnetInputLogLineMax + 1] = {};
size_t inputLogLineLength = 0;
bool inputLogSawCarriageReturn = false;

int oldCursor = 0;
constexpr int screenColumns = AtariScreenColumns;
constexpr int screenBytes = AtariScreenBytes;
bool forceFullScreenRefresh = false;
AtariScreenSnapshot previousTelnetScreen = {};
bool havePreviousTelnetScreen = false;

void resetScreenTracking()
{
    oldCursor = 0;
    forceFullScreenRefresh = true;
    havePreviousTelnetScreen = false;
}

void resetInputLog()
{
    inputLogLineLength = 0;
    inputLogLine[0] = '\0';
    inputLogSawCarriageReturn = false;
}

void logInputLineBytes(const uint8_t *data, size_t len)
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
            LOG("TELNET: %s\n", inputLogLine);
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

// Queue text for the client, sending full buffers on the way; dropped if
// the link stops taking it.
void send(const char *msg)
{
    const uint8_t *data = reinterpret_cast<const uint8_t *>(msg);
    uint32_t len = strlen(msg);
    while (len > 0) {
        const uint32_t n = telnetStream.write(data, len);
        data += n;
        len -= n;
        if (len > 0 && !telnetStream.pump(sidecarHost.sidecar(), kPumpUs))
            return;
    }
}

void updateScreen()
{
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

// The client's keys: hotkeys here, the rest typed (unless read-only).
void handleInput(const uint8_t *data, size_t len)
{
    size_t start = 0;
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
            if (i > start)
                putKeys((const char *)data + start, i - start);

            oldCursor = 0;
            forceFullScreenRefresh = true;
            start = i + 1;
            continue;
        }

        if (data[i] != telnetExitHotkey)
            continue;

        if (i > start)
            putKeys((const char *)data + start, i - start);

        core0_exit(1, "4 Telnet Ctrl-P exit hotkey received");
        telnetStream.close();   // the Sidecar drops the client
        return;
    }

    if (start < len)
        putKeys((const char *)data + start, len - start);
}

}  // namespace

void telnetScreenBegin()
{
    if (streamReady || !sidecarHost.sidecar().present())
        return;
    uint8_t *rx = static_cast<uint8_t *>(heap_caps_malloc(kRxBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    uint8_t *tx = static_cast<uint8_t *>(heap_caps_malloc(kTxBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!rx || !tx) {
        LOG("TELNET: no memory for the stream buffers\n");
        heap_caps_free(rx);
        heap_caps_free(tx);
        return;
    }
    telnetStream.init(static_cast<uint8_t>(SidecarChannel::Telnet), rx, kRxBytes, tx, kTxBytes);
    sidecarHost.sidecar().attachStream(telnetStream);
    streamReady = true;
}

void telnetScreenRun()
{
    if (!streamReady)
        return;
    if (telnetStream.opens() != seenOpens) {
        seenOpens = telnetStream.opens();
        LOG("TELNET: client connected\n");
        resetScreenTracking();
        resetInputLog();
        send(config.telnetServerReadonly
            ? "\r\nCONNECTED! READ ONLY\r\n"
            : "\r\nCONNECTED! READ/WRITE\r\n");
    }
    if (telnetStream.closes() != seenCloses) {
        seenCloses = telnetStream.closes();
        LOG("TELNET: client disconnected\n");
    }
    uint8_t input[64];
    while (uint32_t n = telnetStream.read(input, sizeof(input)))
        handleInput(input, n);
    if (telnetStream.isOpen())
        updateScreen();
    // Send what's queued; a few frames at most per IRQ (a full screen is ~4).
    for (int i = 0; i < 8 && telnetStream.pending(); ++i) {
        if (!telnetStream.pump(sidecarHost.sidecar(), kPumpUs))
            break;
    }
}

#else   // no Sidecar: no telnet
void telnetScreenBegin() {}
void telnetScreenRun() {}
#endif
