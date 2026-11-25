#pragma once
// A text dump of what ANTIC displays, from which tools/screenshot_png.py
// rebuilds a PNG: the ANTIC/GTIA registers (as last written, from
// d000Write[]) and their OS shadows, the display list as a hex dump and
// decoded, the screen memory of each mode line, and the character set when
// text modes are on screen. Read through ANTIC's view of the board's
// memory (PAGESEL_VID in mmuStateSaved): native RAM and ROM the board
// doesn't serve read as "--". Cold bus only (flash code). Format: the
// comment block in screenshot.cpp.
void dumpScreenshot(const char *tag);
#include <cstddef>
// The same records to sink(text, length), one record per call, not LOG().
void dumpScreenshotTo(const char *tag, void (*sink)(const char *text, size_t length));

// Screenshots for the Sidecar (SidecarMainFlag::Screenshot): the flag sets
// screenshotRequested (core0Loop(), after the link poll), and the cold PBI
// handler's next IRQ call sends one (screenshotToSidecar(): dumpScreenshot()'s
// text, one session on the Screenshot stream channel). No-ops without the
// Sidecar. screenshotStreamBegin(): at setup, beside telnetScreenBegin().
extern volatile int screenshotRequested;
void screenshotStreamBegin();
void screenshotToSidecar();
