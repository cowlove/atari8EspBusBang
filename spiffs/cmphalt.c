#include <atari.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FIRST_CHANNEL 1
#define SECOND_CHANNEL 2

static unsigned char cio(unsigned char channel) {
    unsigned char iocbOffset = (unsigned char)(channel << 4);
    __asm__ volatile("tax\n\tjsr $E456" : "+a"(iocbOffset) : : "x", "y", "memory");
    return OS.iocb[channel].status;
}

static unsigned char openFile(unsigned char channel, char *path, unsigned char mode) {
    volatile iocb_t *iocb = &OS.iocb[channel];
    iocb->command = IOCB_OPEN;
    iocb->buffer = path;
    iocb->aux1 = mode;
    iocb->aux2 = 0;
    return cio(channel);
}

static unsigned char closeFile(unsigned char channel) {
    OS.iocb[channel].command = IOCB_CLOSE;
    return cio(channel);
}

static unsigned char readBytes(unsigned char channel, unsigned char *buffer,
                               unsigned int capacity, unsigned int *bytesRead) {
    volatile iocb_t *iocb = &OS.iocb[channel];
    iocb->command = IOCB_GETCHR;
    iocb->buffer = buffer;
    iocb->buflen = capacity;
    {
        unsigned char status = cio(channel);
        *bytesRead = iocb->buflen;
        return status;
    }
}

static unsigned char writeBytes(unsigned char channel, unsigned char *buffer, unsigned int size) {
    volatile iocb_t *iocb = &OS.iocb[channel];
    iocb->command = IOCB_PUTCHR;
    iocb->buffer = buffer;
    iocb->buflen = size;
    return cio(channel);
}

static int writeFile(unsigned char channel, char *path, unsigned char *buffer, unsigned int size) {
    int success;
    if (openFile(channel, path, 8) != 1) return 0;
    success = writeBytes(channel, buffer, size) == 1;
    if (closeFile(channel) != 1) success = 0;
    return success;
}

static int parseCopies(const char *text, unsigned long *copies) {
    unsigned long value = 0;
    if (*text == '\0') return 0;
    while (*text != '\0') {
        unsigned int digit;
        if (*text < '0' || *text > '9') return 0;
        digit = (unsigned int)(*text++ - '0');
        if (value > (ULONG_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    if (value == 0) return 0;
    *copies = value;
    return 1;
}

static int parseCommandLine(char *argv[], char args[][64]) {
    unsigned int comtab = (unsigned int)OS.dosvec;
    volatile unsigned char *line = (volatile unsigned char *)(uintptr_t)(comtab + 63);
    unsigned int i = 0;
    unsigned int argc = 0;
    unsigned int length = 0;
    char commandLine[65];

    while (length < sizeof(commandLine)) {
        unsigned char c = line[length];
        if (c == 0 || c == 0x9B) break;
        commandLine[length++] = (char)c;
    }
    commandLine[length] = '\0';

    while (i < length && commandLine[i] != ' ' && commandLine[i] != '\t') ++i;
    while (i < length) {
        unsigned int out = 0;
        while (i < length && (commandLine[i] == ' ' || commandLine[i] == '\t')) ++i;
        if (i == length) break;
        if (argc == 4) return -1;
        if (commandLine[i] == '"') {
            ++i;
            while (i < length && commandLine[i] != '"') {
                if (out == 63) return -1;
                args[argc][out++] = commandLine[i++];
            }
            if (i == length) return -1;
            ++i;
        } else {
            while (i < length && commandLine[i] != ' ' && commandLine[i] != '\t') {
                if (out == 63) return -1;
                args[argc][out++] = commandLine[i++];
            }
        }
        args[argc][out] = '\0';
        ++argc;
        argv[argc] = args[argc - 1];
    }
    argv[0] = (char *)"CMPHALT";
    return (int)argc + 1;
}

int main(void) {
    enum { IO_BUFFER_SIZE = 2048 };
    char args[4][64];
    char *argv[5];
    int argc = parseCommandLine(argv, args);
    unsigned char *firstBuffer;
    unsigned char *secondBuffer;
    unsigned int firstCount;
    unsigned int secondCount;
    unsigned int compareCount;
    unsigned int i;
    unsigned char firstStatus;
    unsigned char secondStatus;
    unsigned long copies = 1;
    unsigned long copy;
    int result = 0;
    unsigned long position = 0;
    unsigned long mismatchPosition = 0;
    int eofChannel = 0;
    int logOk;
    char report[256];
    unsigned int reportLength;

    if (argc < 3 || argc > 5) {
        puts("Usage: CMPHALT first-file second-file [copies [report-file]]");
        return 2;
    }
    if (argc >= 4 && !parseCopies(argv[3], &copies)) {
        puts("copies must be a positive decimal integer");
        return 2;
    }
    firstBuffer = malloc(IO_BUFFER_SIZE);
    secondBuffer = malloc(IO_BUFFER_SIZE);
    if (firstBuffer == NULL || secondBuffer == NULL) {
        puts("Cannot allocate I/O buffers");
        free(firstBuffer);
        free(secondBuffer);
        return 2;
    }
    if (openFile(FIRST_CHANNEL, argv[1], 4) != 1) {
        puts("Cannot open first file");
        result = 2;
        goto done;
    }
    if (openFile(SECOND_CHANNEL, argv[2], 4) != 1) {
        puts("Cannot open second file");
        closeFile(FIRST_CHANNEL);
        result = 2;
        goto done;
    }

    for (copy = 0; copy < copies && result == 0; ++copy) {
        if (copy != 0 && openFile(FIRST_CHANNEL, argv[1], 4) != 1) {
            puts("Cannot reopen first file");
            result = 2;
            break;
        }
        for (;;) {
            firstStatus = readBytes(FIRST_CHANNEL, firstBuffer, IO_BUFFER_SIZE, &firstCount);
            if (firstStatus != 1 && firstStatus != 136) {
                puts("Error reading first file");
                result = 2;
                break;
            }
            if (firstCount == 0 && firstStatus == 136) break;
            if (firstCount == 0) {
                puts("Empty successful read from first file");
                result = 2;
                break;
            }
            secondStatus = readBytes(SECOND_CHANNEL, secondBuffer, firstCount, &secondCount);
            if (secondStatus != 1 && secondStatus != 136) {
                puts("Error reading second file");
                result = 2;
                break;
            }
            compareCount = firstCount < secondCount ? firstCount : secondCount;
            for (i = 0; i < compareCount; ++i) {
                if (firstBuffer[i] != secondBuffer[i]) {
                    result = 1;
                    mismatchPosition = position + i + 1;
                    break;
                }
            }
            position += compareCount;
            if (result != 0) break;
            if (secondCount < firstCount) {
                result = 1;
                mismatchPosition = position + 1;
                eofChannel = SECOND_CHANNEL;
                break;
            }
            if (secondStatus == 136 && firstStatus != 136) {
                result = 1;
                mismatchPosition = position + 1;
                eofChannel = SECOND_CHANNEL;
                break;
            }
            if (firstStatus == 136) break;
        }
        if (closeFile(FIRST_CHANNEL) != 1 && result == 0) {
            puts("Error closing first file");
            result = 2;
        }
    }

    if (result == 0) {
        secondStatus = readBytes(SECOND_CHANNEL, secondBuffer, 1, &secondCount);
        if (secondStatus == 1 && secondCount != 0) {
            result = 1;
            mismatchPosition = position + 1;
            eofChannel = FIRST_CHANNEL;
        } else if (secondStatus != 136) {
            puts("Error reading second file");
            result = 2;
        }
    }
    if (closeFile(SECOND_CHANNEL) != 1 && result == 0) {
        puts("Error closing second file");
        result = 2;
    }

done:
    free(firstBuffer);
    free(secondBuffer);

    if (result == 0) {
        puts("MATCH");
    } else if (result == 1) {
        if (eofChannel != 0)
            sprintf(report, "%s %s differ: EOF on %s after byte %lu", argv[1], argv[2],
                    eofChannel == FIRST_CHANNEL ? argv[1] : argv[2], position);
        else
            sprintf(report, "%s %s differ: byte %lu", argv[1], argv[2], mismatchPosition);
        puts(report);
        if (argc == 5) {
            reportLength = strlen(report);
            report[reportLength++] = (char)0x9B;
            logOk = writeFile(FIRST_CHANNEL, argv[3], (unsigned char *)report, reportLength);
            if (!logOk) {
                puts("Could not write report file");
                result = 2;
            }
        }
    }
    return result;
}
