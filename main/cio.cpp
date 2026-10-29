#ifdef CSIM
#include "csim.h"
#endif

#include <cstring>
#include <cstdio>
#include <new>
#include "asmDefs.h"
#include "cio.h"
#include "main.h"
#include "log.h"

DRAM_ATTR CioDispatcher *cioFiles;

DRAM_ATTR const char *defaultProgram =
#ifdef BOOT_SDX
        "10 PRINT \"HELLO FROM BASIC\" \233"
        "20 PRINT \"HELLO 2\" \233"
        "30 CLOSE #4:OPEN #4,8,0,\"J1:DUMPSCREEN\":PUT #4,0:CLOSE #4\233"
        "40 DOS\233 "
        "RUN \233"
#else
        //"1 DIM D$(255) \233"
        //"10 REM A=USR(1546, 1) \233"
        //"15 OPEN #1,4,0,\"J2:\" \233"
        //"20 GET #1,A  \233"
        //"38 CLOSE #1  \233"
        //"39 PRINT \"OK\" \233"
        //"40 GOTO 10 \233"
        //"41 OPEN #1,8,0,\"J\" \233"
        //"42 PUT #1,A + 1 \233"
        //"43 CLOSE #1 \233"
        //"50 PRINT \" -> \"; \233"
        //"52 PRINT COUNT; \233"
        //"53 COUNT = COUNT + 1 \233"
        //"60 OPEN #1,8,0,\"D1:DAT\":FOR I=0 TO 20:XIO 11,#1,8,0,D$:NEXT I:CLOSE #1 \233"
        //"61 TRAP 61: CLOSE #1: OPEN #1,4,0,\"D1:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        //"61 CLOSE #1: OPEN #1,4,0,\"D1:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        //"63 OPEN #1,4,0,\"D2:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        "60 TRAP 80:XIO 80,#1,0,0,\"D1:DIR D4:*.*/A\" \233"
        "70 TRAP 80:XIO 80,#1,0,0,\"D1:X.CMD\" \233"
        //"80 GOTO 10 \233"
        "RUN\233"
#endif
        ;

namespace {

bool globMatches(const char *pattern, const char *text) {
    const char *star = nullptr;
    const char *retry = nullptr;
    while (*text) {
        if (*pattern == '?' || *pattern == *text) {
            ++pattern;
            ++text;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = text;
        } else if (star != nullptr) {
            pattern = star + 1;
            text = ++retry;
        } else {
            return false;
        }
    }
    while (*pattern == '*') ++pattern;
    return *pattern == '\0';
}

class ProgramFileHandler final : public CioFileHandler {
public:
    ProgramFileHandler() {
        std::strncpy(buffer, defaultProgram, sizeof(buffer) - 1);
        length = std::strlen(buffer);
    }

    bool open(PbiIocb *, const AtariIOCB *, uint8_t, const std::string &) override {
        position = 0;
        return true;
    }

    void close(PbiIocb *, uint8_t) override {}

    int get(PbiIocb *, uint8_t) override {
        if (position >= length) return -1;
        return static_cast<uint8_t>(buffer[position++]);
    }

    bool put(PbiIocb *, uint8_t, uint8_t) override {
        return false;
    }

private:
    char buffer[256] = {};
    size_t position = 0;
    size_t length = 0;
};

class ScreenDumpHandler final : public CioFileHandler {
public:
    bool open(PbiIocb *pbiRequest, const AtariIOCB *, uint8_t, const std::string &) override {
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        uint8_t *screen = mappedElseCopyIn(pbiRequest, savmsc, 24 * 40);
        if (screen == nullptr) return false;
        dumpScreenToSerial('S', screen);
        lastScreenShot = elapsedSec;
        return true;
    }

    void close(PbiIocb *, uint8_t) override {}
    int get(PbiIocb *, uint8_t) override { return -1; }
    bool put(PbiIocb *, uint8_t, uint8_t) override { return false; }
};

class SerialLogHandler final : public CioFileHandler {
public:
    bool open(PbiIocb *, const AtariIOCB *, uint8_t, const std::string &) override {
        lineLength = 0;
        skipLfAfterCr = false;
        lineOpen = false;
        linePrefixWritten = false;
        return true;
    }

    void close(PbiIocb *, uint8_t) override {
        if (lineOpen) {
            flush(true);
            lineOpen = false;
        }
    }

    int get(PbiIocb *, uint8_t) override { return -1; }

    bool put(PbiIocb *, uint8_t, uint8_t c) override {
        if (c == '\n' && skipLfAfterCr) {
            skipLfAfterCr = false;
            return true;
        }
        skipLfAfterCr = false;

        if (c == '\r' || c == '\n' || c == 155) {
            skipLfAfterCr = c == '\r';
            bool flushed = flush(true);
            lineOpen = false;
            return flushed;
        }

        if (lineLength == sizeof(lineBuffer) && !flush(false)) return false;
        lineBuffer[lineLength++] = static_cast<char>(c);
        lineOpen = true;
        return true;
    }

private:
    bool flush(bool newline) {
        if (!linePrefixWritten && (lineLength != 0 || newline)) {
            static constexpr char prefix[] = "LOG: ";
            if (std::fwrite(prefix, 1, sizeof(prefix) - 1, stdout) != sizeof(prefix) - 1)
                return false;
            linePrefixWritten = true;
        }
        if (lineLength != 0 && std::fwrite(lineBuffer, 1, lineLength, stdout) != lineLength)
            return false;
        lineLength = 0;
        if (newline && std::fputc('\n', stdout) == EOF) return false;
        if (newline) linePrefixWritten = false;
        if ((newline || lineLength == 0) && std::fflush(stdout) != 0) return false;
        return true;
    }

    char lineBuffer[256] = {};
    size_t lineLength = 0;
    bool skipLfAfterCr = false;
    bool lineOpen = false;
    bool linePrefixWritten = false;
};

using HandlerFactory = CioFileHandler *(*)();
struct HandlerRegistration {
    const char *glob;
    HandlerFactory create;
};

std::vector<HandlerRegistration> registrations = {
    {"J1:DUMPSCREEN", []() { return (CioFileHandler *)new (std::nothrow)ScreenDumpHandler(); }},
    {"J1:SCREENDUMP", []() { return (CioFileHandler *)new (std::nothrow)ScreenDumpHandler(); }},
    {"J1:LOG", []() { return (CioFileHandler *)new (std::nothrow)SerialLogHandler(); }},
    {"J:",[]() { return (CioFileHandler *)new (std::nothrow)ProgramFileHandler(); }},
    {"J", []() { return (CioFileHandler *)new (std::nothrow)ProgramFileHandler(); }}, 
    {"J?:",[]() { return (CioFileHandler *)new (std::nothrow)ProgramFileHandler(); }},
};

} // namespace

void registerCioFileHandler(const char *glob, HandlerFactory factory) { 
    registrations.push_back({glob, factory});
}

CioDispatcher::CioDispatcher() : openFiles{}, openPendingCopyIn{}, openFilenameValid{}, openFilenames{} {}

bool CioDispatcher::open(PbiIocb *pbiRequest, const AtariIOCB *iocb, uint8_t iocbNumber) {
    if (iocbNumber >= sizeof(openFiles) / sizeof(openFiles[0])) return false;

    if (!openFilenameValid[iocbNumber]) {
        uint16_t filenameAddress = (static_cast<uint16_t>(iocb->ICBAH) << 8) | iocb->ICBAL;
        uint8_t *filenameBytes = mappedElseCopyIn(pbiRequest, filenameAddress, 32);
        if (filenameBytes == nullptr) return false;
        size_t filenameLength = 0;
        while (filenameLength < 32 && filenameBytes[filenameLength] != 155) ++filenameLength;
        openFilenames[iocbNumber].assign(reinterpret_cast<const char *>(filenameBytes), filenameLength);
        openFilenameValid[iocbNumber] = true;
    }
    const std::string &filename = openFilenames[iocbNumber];

    CioFileHandler *handler = nullptr;
    if (openPendingCopyIn[iocbNumber]) {
        handler = openFiles[iocbNumber];
    } else {
        if (openFiles[iocbNumber] != nullptr) {
            openFiles[iocbNumber]->close(pbiRequest, iocbNumber);
            if ((pbiRequest->result & RES_FLAG_NEED_COPYIN) != 0) return false;
            delete openFiles[iocbNumber];
            openFiles[iocbNumber] = nullptr;
        }
        openPendingCopyIn[iocbNumber] = false;
        for (const HandlerRegistration &registration : registrations) {
            if (globMatches(registration.glob, filename.c_str())) {
                handler = registration.create();
                break;
            }
        }
        if (handler == nullptr) handler = new (std::nothrow) ProgramFileHandler();
        if (handler == nullptr) return false;
        openFiles[iocbNumber] = handler;
    }

    bool result = handler->open(pbiRequest, iocb, iocbNumber, filename);
    openPendingCopyIn[iocbNumber] = (pbiRequest->result & RES_FLAG_NEED_COPYIN) != 0;
    if (openPendingCopyIn[iocbNumber]) return result;
    openFilenameValid[iocbNumber] = false;
    structLogs->opens.add(filename.c_str());
    if (!result && !openPendingCopyIn[iocbNumber]) {
        handler->close(pbiRequest, iocbNumber);
        delete handler;
        openFiles[iocbNumber] = nullptr;
    }
    return result;
}

void CioDispatcher::close(PbiIocb *pbiRequest, uint8_t iocbNumber) {
    if (iocbNumber >= sizeof(openFiles) / sizeof(openFiles[0])) return;
    CioFileHandler *handler = openFiles[iocbNumber];
    if (handler == nullptr) return;
    handler->close(pbiRequest, iocbNumber);
    if ((pbiRequest->result & RES_FLAG_NEED_COPYIN) != 0) return;
    delete handler;
    openFiles[iocbNumber] = nullptr;
    openPendingCopyIn[iocbNumber] = false;
    openFilenameValid[iocbNumber] = false;
}

int CioDispatcher::get(PbiIocb *pbiRequest, uint8_t iocbNumber) {
    if (iocbNumber >= sizeof(openFiles) / sizeof(openFiles[0]) || openFiles[iocbNumber] == nullptr) return -1;
    return openFiles[iocbNumber]->get(pbiRequest, iocbNumber);
}

bool CioDispatcher::put(PbiIocb *pbiRequest, uint8_t iocbNumber, uint8_t c) {
    if (iocbNumber >= sizeof(openFiles) / sizeof(openFiles[0]) || openFiles[iocbNumber] == nullptr) return false;
    return openFiles[iocbNumber]->put(pbiRequest, iocbNumber, c);
}
