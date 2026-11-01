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
        if ((row[column] & 0x7f) != 0)
            return false;
    }
    return true;
}

AtariScreenAlignment findAtariScreenAlignment(const AtariScreenSnapshot &previous,
                                               const AtariScreenSnapshot &current)
{
    bool rowMatches[AtariScreenRows][AtariScreenRows] = {};
    int previousRowOccurrences[AtariScreenRows] = {};
    int currentRowOccurrences[AtariScreenRows] = {};
    for (int previousRow = 0; previousRow < AtariScreenRows; ++previousRow) {
        for (int currentRow = 0; currentRow < AtariScreenRows; ++currentRow) {
            rowMatches[previousRow][currentRow] =
                screenRowsEqual(previous, previousRow, current, currentRow);
            previousRowOccurrences[previousRow] +=
                screenRowsEqual(previous, previousRow, previous, currentRow);
            currentRowOccurrences[currentRow] +=
                screenRowsEqual(current, currentRow, current, previousRow);
        }
    }

    AtariScreenAlignment best{0, 0, false};
    int bestUniqueAnchors = -1;
    int bestNonblankRows = -1;
    int bestMatchedCharacters = -1;
    int bestExactRows = -1;
    const int currentCompletedRows = current.cursorRow;

    if (current.cursorRow < previous.cursorRow)
        return best;

    for (int shift = 0; shift < previous.cursorRow; ++shift) {
        const int availablePreviousRows = previous.cursorRow - shift;
        const int limit = availablePreviousRows < currentCompletedRows ?
                          availablePreviousRows : currentCompletedRows;
        int matchedRows = 0;
        int matchedNonblankRows = 0;
        int matchedCharacters = 0;
        int exactRows = 0;
        int uniqueAnchors = 0;
        while (matchedRows < limit && rowMatches[shift + matchedRows][matchedRows])
            ++matchedRows;

        for (int currentRow = 0; currentRow < limit; ++currentRow) {
            const int previousRow = shift + currentRow;
            const uint8_t *previousCells =
                previous.cells + previousRow * AtariScreenColumns;
            const uint8_t *currentCells =
                current.cells + currentRow * AtariScreenColumns;
            const bool currentBlank = screenRowIsBlank(currentCells);
            const bool previousBlank = screenRowIsBlank(previousCells);

            if (currentBlank || previousBlank)
                continue;

            ++matchedNonblankRows;
            if (rowMatches[previousRow][currentRow]) {
                ++exactRows;
                if (previousRowOccurrences[previousRow] == 1 &&
                    currentRowOccurrences[currentRow] == 1)
                    ++uniqueAnchors;
            }

            for (int column = 0; column < AtariScreenColumns; ++column) {
                const uint8_t previousCharacter = previousCells[column] & 0x7f;
                const uint8_t currentCharacter = currentCells[column] & 0x7f;
                if (previousCharacter != 0 &&
                    previousCharacter == currentCharacter)
                    ++matchedCharacters;
            }
        }

        if (matchedRows == 0 || matchedNonblankRows == 0 || matchedCharacters == 0)
            continue;

        const bool distinctiveMatch = uniqueAnchors > 0 &&
            (matchedRows == 1 || matchedNonblankRows >= 2 || matchedRows >= 4);
        if (!distinctiveMatch)
            continue;

        if (matchedCharacters > bestMatchedCharacters ||
            (matchedCharacters == bestMatchedCharacters && uniqueAnchors > bestUniqueAnchors) ||
            (matchedCharacters == bestMatchedCharacters && uniqueAnchors == bestUniqueAnchors &&
             exactRows > bestExactRows) ||
            (matchedCharacters == bestMatchedCharacters && uniqueAnchors == bestUniqueAnchors &&
             exactRows == bestExactRows && matchedRows > best.matchedRows) ||
            (matchedCharacters == bestMatchedCharacters && uniqueAnchors == bestUniqueAnchors &&
             exactRows == bestExactRows && matchedRows == best.matchedRows &&
             matchedNonblankRows > bestNonblankRows) ||
            (matchedCharacters == bestMatchedCharacters && uniqueAnchors == bestUniqueAnchors &&
             exactRows == bestExactRows && matchedRows == best.matchedRows &&
             matchedNonblankRows == bestNonblankRows && shift < best.scrollRows)) {
            best = {shift, matchedRows, true};
            bestUniqueAnchors = uniqueAnchors;
            bestNonblankRows = matchedNonblankRows;
            bestMatchedCharacters = matchedCharacters;
            bestExactRows = exactRows;
        }
    }

    return best;
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
    const AtariScreenAlignment alignment = findAtariScreenAlignment(previous, current);
    const int scrollRows = alignment.confident ? alignment.scrollRows : 0;
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
        for (int row = alignment.matchedRows; row < current.cursorRow; ++row)
            pendingRows[row] = true;
    } else if (alignment.confident) {
        for (int row = alignment.matchedRows; row < current.cursorRow; ++row)
            pendingRows[row] = true;
    } else {
        for (int row = previous.cursorRow; row <= current.cursorRow &&
             row < AtariScreenRows; ++row) {
            if (!screenRowsEqual(previous, row, current, row))
                pendingRows[row] = true;
        }
    }

    for (int row = 0; row < current.cursorRow; ++row) {
        if (pendingRows[row]) {
            printScreenRow(current.cells + row * AtariScreenColumns);
            pendingRows[row] = false;
        }
    }

    previous = current;
}
