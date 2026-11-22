// dumpScreenshot(): what ANTIC displays, as text that tools/screenshot_png.py
// turns into a PNG (screenshot.h). For regression boots of programs that
// never use the PBI device or the console (cartridge games): the shutdown
// report carries their screen.
//
// Format, one record per line, fields separated by single spaces, all
// numbers hex (addresses 4 digits, bytes 2) except the counts after
// "blank", "lines" and "bytes", which are decimal:
//
//   screenshot: begin <tag>
//   antic: dmactl XX chactl XX dlist XXXX hscrol XX vscrol XX pmbase XX chbase XX nmien XX
//   gtia: colpm XX XX XX XX colpf XX XX XX XX colbk XX prior XX
//   shadow: sdmctl XX chact XX sdlst XXXX chbas XX pcolr XX XX XX XX color XX XX XX XX XX gprior XX
//   dlist: AAAA XX XX ...                 the display list's bytes as walked,
//                                         up to 16 a line; a new line at a jump
//   dl: AAAA XX blank N [dli]             N blank scan lines
//   dl: AAAA XX mode M lines N bytes B [lms AAAA] [dli] [hscrol] [vscrol]
//                                         a mode line: M (2-f) the ANTIC mode,
//                                         N scan lines, B bytes fetched; the
//                                         next screenmem record is its data
//   screenmem: AAAA XX XX ...             that mode line's B bytes, from AAAA
//                                         (the 4 KB counter wraps inside its
//                                         block, as ANTIC's does)
//   dl: AAAA XX jmp AAAA                  a jump (the walk follows it)
//   dl: AAAA XX jvb AAAA                  jump and wait for VBL: the end
//   dl: AAAA end <reason>                 the walk stopped (unreadable byte,
//                                         too many lines or instructions)
//   charset: AAAA XX XX ...               the character set (32 bytes a line):
//                                         1 KB at CHBASE & $FC for modes 2-5,
//                                         512 bytes at CHBASE & $FE for 6-7
//   screenshot: end <tag>
//
// A byte the board doesn't serve (native RAM, the Atari's own ROM) is "--".
// The registers are their last written values (core 1 keeps every $D000-$D7FF
// write in d000Write[]), so a display list interrupt's mid-screen changes
// show only as whatever it wrote last. No player/missile graphics.

#ifdef CSIM
#include "csim.h"
#endif

#include "screenshot.h"

#include <cstdio>
#include <cstring>

#include "a8log.h"
#include "main.h"
#include "mmu.h"

namespace {

// The byte ANTIC would fetch at addr, if the board serves that page.
bool peek(uint16_t addr, uint8_t &value) {
    const int p = pageNr(addr);
    L1Block *b = mmuBlock(mmuStateSaved, page2L1Block(p));
    const int i = (p & pageInL1BlockMask) | PAGESEL_RD | PAGESEL_CPU;
    if (b == nullptr || b->ctrl[i] == 0 || b->pages[i] == nullptr)
        return false;
    value = b->pages[i][addr & pageOffsetMask];
    return true;
}

// A record of hex bytes: "<prefix>: AAAA XX XX ...", built in a buffer and
// logged once. step(addr) is the next address (ANTIC's counters wrap).
template <typename Step>
void hexRecord(const char *prefix, uint16_t addr, int count, Step step) {
    char line[16 + 3 * 64];
    int n = snprintf(line, sizeof(line), "%s: %04x", prefix, addr);
    for (int i = 0; i < count && n < (int)sizeof(line) - 4; i++) {
        uint8_t v;
        if (peek(addr, v))
            n += snprintf(line + n, sizeof(line) - n, " %02x", v);
        else
            n += snprintf(line + n, sizeof(line) - n, " --");
        addr = step(addr);
    }
    LOG("%s\n", line);
}

void hexDump(const char *prefix, uint16_t addr, int count, int perLine) {
    for (int done = 0; done < count; done += perLine) {
        const int n = count - done < perLine ? count - done : perLine;
        hexRecord(prefix, uint16_t(addr + done), n, [](uint16_t a) { return uint16_t(a + 1); });
    }
}

// ANTIC's display list counter: 10 bits, the top 6 fixed (a list can't
// cross a 1 KB boundary without a jump).
uint16_t dlNext(uint16_t a) { return (a & 0xfc00) | ((a + 1) & 0x03ff); }
// Its memory scan counter: 12 bits, the top 4 fixed.
uint16_t msAdd(uint16_t a, int n) { return (a & 0xf000) | ((a + n) & 0x0fff); }

// Bytes a mode line fetches at normal width (40 columns of 4 color clocks).
constexpr int kNormalBytes[16] = {0, 0, 40, 40, 40, 40, 20, 20, 10, 10, 20, 20, 20, 40, 40, 40};
constexpr int kScanLines[16] = {0, 0, 8, 10, 8, 16, 8, 16, 8, 4, 4, 2, 1, 2, 1, 1};

int lineBytes(int mode, uint8_t dmactl, bool hscrol) {
    int width = dmactl & 3;   // 0 no playfield, 1 narrow, 2 normal, 3 wide
    if (width == 0)
        return 0;
    if (hscrol && width < 3)
        width++;   // horizontal scrolling fetches the next wider playfield
    const int normal = kNormalBytes[mode];
    return width == 1 ? normal * 4 / 5 : width == 2 ? normal : normal * 6 / 5;
}

constexpr int kMaxInstructions = 512;
constexpr int kMaxScanLines = 300;   // more than a frame's 240 visible

struct Walk {
    bool textModes = false;   // 2-5: a 1 KB character set
    bool textModes67 = false;   // 6-7: 512 bytes
};

// One walk of the display list from dlist. dump: the "dlist:" records (the
// bytes in segments, a new one at each jump); otherwise the decoded "dl:"
// and "screenmem:" records.
Walk walkDisplayList(uint16_t dlist, uint8_t dmactl, bool dump) {
    Walk w;
    uint16_t pc = dlist;
    uint16_t ms = 0;
    int scanLines = 0;
    // dump: the current segment's start and length.
    uint16_t segStart = pc;
    int segLen = 0;
    auto flushSegment = [&]() {
        if (dump && segLen > 0) {
            for (int done = 0; done < segLen; done += 16) {
                uint16_t a = segStart;
                for (int i = 0; i < done; i++) a = dlNext(a);
                hexRecord("dlist", a, segLen - done < 16 ? segLen - done : 16, dlNext);
            }
        }
        segLen = 0;
    };
    auto fetch = [&](uint8_t &v) {
        const bool ok = peek(pc, v);
        if (ok) {
            pc = dlNext(pc);
            segLen++;
        }
        return ok;
    };
    for (int n = 0; n < kMaxInstructions; n++) {
        const uint16_t at = pc;
        uint8_t ir;
        if (!fetch(ir)) {
            flushSegment();
            if (!dump) LOG("dl: %04x end unreadable\n", at);
            return w;
        }
        const int mode = ir & 0x0f;
        const bool dli = (ir & 0x80) != 0;
        if (mode == 0) {
            const int lines = ((ir >> 4) & 7) + 1;
            if (!dump) LOG("dl: %04x %02x blank %d%s\n", at, ir, lines, dli ? " dli" : "");
            scanLines += lines;
        } else if (mode == 1) {
            uint8_t lo, hi;
            if (!fetch(lo) || !fetch(hi)) {
                flushSegment();
                if (!dump) LOG("dl: %04x end unreadable\n", at);
                return w;
            }
            const uint16_t target = uint16_t(lo | (hi << 8));
            const bool jvb = (ir & 0x40) != 0;
            if (!dump) LOG("dl: %04x %02x %s %04x%s\n", at, ir, jvb ? "jvb" : "jmp", target, dli ? " dli" : "");
            flushSegment();
            if (jvb)
                return w;
            pc = segStart = target;
            if (!dump) scanLines += 1;   // a jump shows one blank line
            continue;
        } else {
            char lms[16] = "";
            if (ir & 0x40) {
                uint8_t lo, hi;
                if (!fetch(lo) || !fetch(hi)) {
                    flushSegment();
                    if (!dump) LOG("dl: %04x end unreadable\n", at);
                    return w;
                }
                ms = uint16_t(lo | (hi << 8));
                snprintf(lms, sizeof(lms), " lms %04x", ms);
            }
            const bool hscrol = (ir & 0x10) != 0;
            const int bytes = lineBytes(mode, dmactl, hscrol);
            const int lines = kScanLines[mode];
            if (mode <= 5) w.textModes = true;
            else if (mode <= 7) w.textModes67 = true;
            if (!dump) {
                LOG("dl: %04x %02x mode %x lines %d bytes %d%s%s%s%s\n", at, ir, mode, lines, bytes, lms,
                    dli ? " dli" : "", hscrol ? " hscrol" : "", (ir & 0x20) ? " vscrol" : "");
                if (bytes > 0)
                    hexRecord("screenmem", ms, bytes, [](uint16_t a) { return msAdd(a, 1); });
            }
            ms = msAdd(ms, bytes);
            scanLines += lines;
        }
        if (scanLines > kMaxScanLines) {
            flushSegment();
            if (!dump) LOG("dl: %04x end %d scan lines\n", pc, scanLines);
            return w;
        }
    }
    flushSegment();
    if (!dump) LOG("dl: %04x end %d instructions\n", pc, kMaxInstructions);
    return w;
}

}  // namespace

void dumpScreenshot(const char *tag) {
    const RAM_VOLATILE uint8_t *hw = d000Write;   // as last written
    const uint8_t dmactl = hw[0x400];
    const uint16_t dlist = uint16_t(hw[0x402] | (hw[0x403] << 8));
    const uint8_t chbase = hw[0x409];
    LOG("screenshot: begin %s\n", tag);
    LOG("antic: dmactl %02x chactl %02x dlist %04x hscrol %02x vscrol %02x pmbase %02x chbase %02x nmien %02x\n",
        dmactl, hw[0x401], dlist, hw[0x404], hw[0x405], hw[0x407], chbase, hw[0x40e]);
    LOG("gtia: colpm %02x %02x %02x %02x colpf %02x %02x %02x %02x colbk %02x prior %02x\n", hw[0x012], hw[0x013],
        hw[0x014], hw[0x015], hw[0x016], hw[0x017], hw[0x018], hw[0x019], hw[0x01a], hw[0x01b]);
    LOG("shadow: sdmctl %02x chact %02x sdlst %04x chbas %02x pcolr %02x %02x %02x %02x "
        "color %02x %02x %02x %02x %02x gprior %02x\n",
        atariRam[559], atariRam[755], atariRam[560] | (atariRam[561] << 8), atariRam[756], atariRam[704],
        atariRam[705], atariRam[706], atariRam[707], atariRam[708], atariRam[709], atariRam[710], atariRam[711],
        atariRam[712], atariRam[623]);
    walkDisplayList(dlist, dmactl, true);
    const Walk w = walkDisplayList(dlist, dmactl, false);
    if (w.textModes)
        hexDump("charset", uint16_t((chbase & 0xfc) << 8), 1024, 32);
    else if (w.textModes67)
        hexDump("charset", uint16_t((chbase & 0xfe) << 8), 512, 32);
    LOG("screenshot: end %s\n", tag);
}
