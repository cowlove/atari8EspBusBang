#ifdef CSIM
#include "csim.h"
#endif

#include "screenScrape.h"

#include <cstdio>
#include <cstring>

#include "main.h"
#include "mmu.h"
#include "util.h"

AtariScreenReadResult readAtariScreen(AtariScreenSnapshot &snapshot)
{
    const uint32_t address = (static_cast<uint32_t>(atariRam[89]) << 8) | atariRam[88];
    if (address + AtariScreenBytes > UINT16_MAX + 1U)
        return AtariScreenReadResult::AddressOutOfRange;

    const uint8_t cursorRow = atariRam[84];
    const uint8_t cursorColumn = atariRam[85];
    if (cursorRow >= AtariScreenRows || cursorColumn >= AtariScreenColumns)
        return AtariScreenReadResult::InvalidCursor;

    uint8_t *memory = mmuCheckRangeMapped(mmuStateSaved, static_cast<uint16_t>(address), AtariScreenBytes);
    if (memory == nullptr)
        return AtariScreenReadResult::NotMapped;

    uint8_t verify[AtariScreenBytes];
    for (int attempt = 0; attempt < 2; ++attempt) {
        memcpy(snapshot.cells, memory, AtariScreenBytes);
        memcpy(verify, memory, AtariScreenBytes);
        if (memcmp(snapshot.cells, verify, AtariScreenBytes) == 0) {
            snapshot.address = static_cast<uint16_t>(address);
            snapshot.cursorRow = cursorRow;
            snapshot.cursorColumn = cursorColumn;
            return AtariScreenReadResult::Ok;
        }
    }

    return AtariScreenReadResult::Unstable;
}

static bool screenRowsEqual(const AtariScreenSnapshot &left, int leftRow,
                            const AtariScreenSnapshot &right, int rightRow)
{
    return memcmp(left.cells + leftRow * AtariScreenColumns,
                  right.cells + rightRow * AtariScreenColumns,
                  AtariScreenColumns) == 0;
}

static bool screenRowIsBlank(const uint8_t *row)
{
    for (int column = 0; column < AtariScreenColumns; ++column) {
        if (row[column] != 0 && row[column] != ' ')
            return false;
    }
    return true;
}

static bool screenRowIsUnique(const AtariScreenSnapshot &screen, int row)
{
    int occurrences = 0;
    for (int otherRow = 0; otherRow < AtariScreenRows; ++otherRow) {
        if (screenRowsEqual(screen, row, screen, otherRow) && ++occurrences > 1)
            return false;
    }
    return occurrences == 1;
}

int findAtariScreenScrollRows(const AtariScreenSnapshot &previous,
                              const AtariScreenSnapshot &current)
{
    int bestShift = 0;
    int bestScore = 0;
    for (int shift = 1; shift < AtariScreenRows; ++shift) {
        int overlap = AtariScreenRows - shift;
        int matchingRows = 0;
        int nonblankMatches = 0;
        int uniqueAnchors = 0;
        int mismatches = 0;
        int longestMatchRun = 0;
        int currentMatchRun = 0;
        for (int row = 0; row < AtariScreenRows - shift; ++row) {
            if (screenRowsEqual(previous, row + shift, current, row)) {
                ++matchingRows;
                ++currentMatchRun;
                if (currentMatchRun > longestMatchRun)
                    longestMatchRun = currentMatchRun;
                const uint8_t *cells = current.cells + row * AtariScreenColumns;
                if (!screenRowIsBlank(cells)) {
                    ++nonblankMatches;
                    if (screenRowIsUnique(previous, row + shift) &&
                        screenRowIsUnique(current, row))
                        ++uniqueAnchors;
                }
            } else {
                ++mismatches;
                currentMatchRun = 0;
            }
        }

        bool exposedRowsChanged = false;
        for (int row = AtariScreenRows - shift; row < AtariScreenRows; ++row) {
            if (!screenRowsEqual(previous, row, current, row)) {
                exposedRowsChanged = true;
                break;
            }
        }

        const int minimumMatches = overlap <= 2 ? 1 :
                                   overlap <= 4 ? 2 : (overlap * 2 + 2) / 3;
        const bool shortOverlapHasAnchor = overlap > 4 || uniqueAnchors > 0;
        if (!exposedRowsChanged || matchingRows < minimumMatches ||
            longestMatchRun < (overlap < 4 ? 1 : 2) ||
            nonblankMatches == 0 || !shortOverlapHasAnchor)
            continue;

        int score = uniqueAnchors * 100 + nonblankMatches * 8 + matchingRows * 2 - mismatches - shift;
        if (score > bestScore) {
            bestScore = score;
            bestShift = shift;
        }
    }
    return bestShift;
}

bool atariScreenNeedsFullResync(const AtariScreenSnapshot &previous,
                                const AtariScreenSnapshot &current)
{
    int changedRows = 0;
    for (int row = 0; row < AtariScreenRows; ++row) {
        if (!screenRowsEqual(previous, row, current, row))
            ++changedRows;
    }

    return current.cursorRow >= AtariScreenRows - 4 &&
           previous.cursorRow >= AtariScreenRows - 4 &&
           changedRows >= AtariScreenRows * 3 / 4;
}

static void printScreenRow(const uint8_t *row)
{
    char line[AtariScreenColumns + 1];
    for (int column = 0; column < AtariScreenColumns; ++column) {
        char character[2];
        screenMemToAscii(character, sizeof(character), row[column], false);
        line[column] = character[0];
    }
    line[AtariScreenColumns] = '\0';

    int length = AtariScreenColumns;
    while (length > 0 && line[length - 1] == ' ')
        --length;
    if (length == 0)
        return;

    printf("ATARI: %.*s\n", length, line);
}

void logNewAtariScreenLines()
{
    static AtariScreenSnapshot previous = {};
    static bool havePrevious = false;
    static bool pendingRows[AtariScreenRows] = {};

    AtariScreenSnapshot current;
    if (readAtariScreen(current) != AtariScreenReadResult::Ok) {
        havePrevious = false;
        return;
    }

    if (!havePrevious || previous.address != current.address) {
        previous = current;
        havePrevious = true;
        memset(pendingRows, 0, sizeof(pendingRows));
        return;
    }

    const bool screenChanged = memcmp(previous.cells, current.cells, AtariScreenBytes) != 0;
    const int scrollRows = screenChanged ? findAtariScreenScrollRows(previous, current) : 0;
    if (screenChanged && scrollRows == 0 && atariScreenNeedsFullResync(previous, current)) {
        for (int row = 0; row < current.cursorRow; ++row)
            printScreenRow(current.cells + row * AtariScreenColumns);
        memset(pendingRows, 0, sizeof(pendingRows));
        previous = current;
        return;
    }

    if (scrollRows > 0) {
        for (int row = 0; row < AtariScreenRows - scrollRows; ++row)
            pendingRows[row] = pendingRows[row + scrollRows];
        for (int row = AtariScreenRows - scrollRows; row < AtariScreenRows; ++row)
            pendingRows[row] = false;
    }

    int firstChangedRow = scrollRows > 0 ? AtariScreenRows - scrollRows : previous.cursorRow;
    for (int row = firstChangedRow; row <= current.cursorRow && row < AtariScreenRows; ++row) {
        if (!screenRowsEqual(previous, row, current, row))
            pendingRows[row] = true;
    }

    for (int row = 0; row < current.cursorRow; ++row) {
        if (pendingRows[row]) {
            printScreenRow(current.cells + row * AtariScreenColumns);
            pendingRows[row] = false;
        }
    }

    previous = current;
}
