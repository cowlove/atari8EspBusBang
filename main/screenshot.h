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
