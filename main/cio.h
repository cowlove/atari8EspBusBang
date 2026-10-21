#pragma once

#include <cstdint>
#include <string>
#include "pbi.h"
#include "main.h"

class CioFileHandler {
public:
    virtual ~CioFileHandler() = default;
    virtual bool open(PbiIocb *pbiRequest, const AtariIOCB *iocb, uint8_t iocbNumber, const std::string &filename) = 0;
    virtual void close(PbiIocb *pbiRequest, uint8_t iocbNumber) = 0;
    virtual int get(PbiIocb *pbiRequest, uint8_t iocbNumber) = 0;
    virtual bool put(PbiIocb *pbiRequest, uint8_t iocbNumber, uint8_t c) = 0;
};

class CioDispatcher {
public:
    CioDispatcher();
    bool open(PbiIocb *pbiRequest, const AtariIOCB *iocb, uint8_t iocbNumber);
    void close(PbiIocb *pbiRequest, uint8_t iocbNumber);
    int get(PbiIocb *pbiRequest, uint8_t iocbNumber);
    bool put(PbiIocb *pbiRequest, uint8_t iocbNumber, uint8_t c);

private:
    CioFileHandler *openFiles[8];
    bool openPendingCopyIn[8];
    bool openFilenameValid[8];
    std::string openFilenames[8];
};

extern DRAM_ATTR CioDispatcher *cioFiles;
