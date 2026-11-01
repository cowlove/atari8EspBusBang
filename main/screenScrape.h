#pragma once

#include <stdint.h>

static constexpr int AtariScreenRows = 24;
static constexpr int AtariScreenColumns = 40;
static constexpr int AtariScreenBytes = AtariScreenRows * AtariScreenColumns;

enum class AtariScreenReadResult {
    Ok,
    AddressOutOfRange,
    NotMapped,
    InvalidCursor,
    Unstable
};

struct AtariScreenSnapshot {
    uint16_t address;
    uint8_t cursorRow;
    uint8_t cursorColumn;
    uint8_t cells[AtariScreenBytes];
};

AtariScreenReadResult readAtariScreen(AtariScreenSnapshot &snapshot);
int findAtariScreenScrollRows(const AtariScreenSnapshot &previous,
                              const AtariScreenSnapshot &current);
bool atariScreenNeedsFullResync(const AtariScreenSnapshot &previous,
                                const AtariScreenSnapshot &current);
void logNewAtariScreenLines();
