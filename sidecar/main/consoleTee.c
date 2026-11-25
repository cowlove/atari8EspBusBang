// The web console's copy of stdout (sidecarApp.cpp, sidecarConsoleTeeBegin()):
// a stdout stream built as IDF builds its own (picolibc's bufio, line
// buffered: each line goes out at its newline), whose write hands the bytes
// to a sink (the console ring) before writing them to the console's fd as
// before. C, for picolibc's stdio-bufio.h. funopen()'s stream wasn't line
// buffered (setvbuf(_IOLBF) had no effect: 256-byte blocks), so a line
// could sit unsent until more output came.
#include <stdint.h>
#include <stdio.h>
#include <stdio-bufio.h>
#include <unistd.h>

static void (*s_sink)(const char *data, size_t length);
static char s_buf[256];

static ssize_t tee_write(int fd, const void *buf, size_t count)
{
    if (s_sink) s_sink((const char *)buf, count);
    const char *p = (const char *)buf;
    size_t left = count;
    while (left > 0) {
        const ssize_t w = write(fd, p, left);
        if (w <= 0) break;   // the console dropped it (no host): the ring still has it
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)count;
}

static ssize_t tee_read(int fd, void *buf, size_t count)
{
    (void)fd; (void)buf; (void)count;
    return -1;
}

static __off_t tee_lseek(int fd, __off_t offset, int whence)
{
    (void)fd; (void)offset; (void)whence;
    return -1;
}

static int tee_close(int fd)
{
    (void)fd;
    return 0;
}

static struct __file_bufio s_tee =
    FDEV_SETUP_BUFIO(1, s_buf, sizeof(s_buf), tee_read, tee_write, tee_lseek, tee_close, __SWR, __BLBF);

void console_tee_install(void (*sink)(const char *data, size_t length))
{
    fflush(stdout);
    const int fd = fileno(stdout);
    s_tee.ptr = (const void *)(intptr_t)(fd >= 0 ? fd : 1);
    s_sink = sink;
    stdout = &s_tee.xfile.cfile.file;
}
