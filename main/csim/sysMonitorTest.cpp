#include <stdio.h>
#include "csim.h"
#include "util.h"
#include "sysMonitor.h"
#include "sysMonitor.cpp"

static const int savmsc = 0x4000;

void printScreen() { 
    char tag = 'X';
    uint8_t *mem = &atariRam[savmsc];
    printf("\033[H");
    printf(DRAM_STR("SCREEN%c 00 memory at SAVMSC(%04x):\n"), tag, savmsc);
    printf(DRAM_STR("SCREEN%c 01 +----------------------------------------+\n"), tag);
    for(int row = 0; row < 24; row++) { 
        printf(DRAM_STR("SCREEN%c %02d |"), tag, row + 2);
        for(int col = 0; col < 40; col++) { 
            uint8_t c = *(mem + row * 40 + col);
            char buf[16];
            screenMemToAscii(buf, sizeof(buf), c);
            printf("%s", buf);
        }
        printf(DRAM_STR("|\n"));
    }
    printf(DRAM_STR("SCREEN%c 27 +----------------------------------------+\n"), tag);
}

#include <stdio.h>
#include <termios.h>
#include <unistd.h> // For STDIN_FILENO

int getch_unbuffered() {
    struct termios oldattr, newattr;
    int ch;

    // Get current terminal attributes
    tcgetattr(STDIN_FILENO, &oldattr);

    // Create new attributes based on old ones
    newattr = oldattr;

    // Disable canonical mode (ICANON) and echo (ECHO)
    newattr.c_lflag &= ~(ICANON | ECHO);

    // Set minimum number of characters for non-canonical read (VMIN) to 1
    newattr.c_cc[VMIN] = 1;

    // Set timeout for non-canonical read (VTIME) to 0 (no timeout)
    newattr.c_cc[VTIME] = 0;

    // Apply the new terminal attributes immediately
    tcsetattr(STDIN_FILENO, TCSANOW, &newattr);

    // Read a single character
    ch = getchar();

    // Restore original terminal attributes
    tcsetattr(STDIN_FILENO, TCSANOW, &oldattr);

    return ch;
}

int main() { 
    printf("\033[2J");
    atariRam[88] = savmsc & 0xff;
    atariRam[89] = savmsc >> 8;
    int c;
    PbiIocb pbi = {0};
    sysMonitor.pbiRequest = &pbi;
    do { 
        sysMonitor.drawScreen();
        printScreen();
        c = getch_unbuffered();
        printf("You pressed ASCII key: %d)\n", c);
        if (c == '\x1B') {
            getch_unbuffered();
            c = getch_unbuffered();
                
            if (c == 'A') sysMonitor.onConsoleKey(3);
            else if (c == 'B') sysMonitor.onConsoleKey(6);
            else if (c == 'C') sysMonitor.onConsoleKey(5);
            continue;
        } else if (c == 32) 
            sysMonitor.onConsoleKey(5);
        else 
            sysMonitor.onKey(c);
    } while (c != 96);
    return 0;
}

uint32_t XTHAL_GET_CCOUNT() { return 0; }