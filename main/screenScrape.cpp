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

int findAtariScreenScrollRows(const AtariScreenSnapshot &previous,
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

    struct ScrollHypothesis {
        int shift = 0;
        int overlap = 0;
        int matchingRows = 0;
        int matchingNonblankRows = 0;
        int uniqueAnchors = 0;
        int longestMatchRun = 0;
        int supportScore = 0;
        bool exposedRowsChanged = false;
        bool confident = false;
    } bestHypothesis;

    for (int shift = 1; shift < AtariScreenRows; ++shift) {
        ScrollHypothesis hypothesis;
        hypothesis.shift = shift;
        hypothesis.overlap = AtariScreenRows - shift;
        int currentMatchRun = 0;
        for (int row = 0; row < AtariScreenRows - shift; ++row) {
            const int previousRow = row + shift;
            if (rowMatches[previousRow][row]) {
                ++hypothesis.matchingRows;
                ++currentMatchRun;
                if (currentMatchRun > hypothesis.longestMatchRun)
                    hypothesis.longestMatchRun = currentMatchRun;
                const uint8_t *cells = current.cells + row * AtariScreenColumns;
                if (!screenRowIsBlank(cells)) {
                    ++hypothesis.matchingNonblankRows;
                    if (previousRowOccurrences[previousRow] == 1 &&
                        currentRowOccurrences[row] == 1)
                        ++hypothesis.uniqueAnchors;
                }
            } else {
                currentMatchRun = 0;
            }
        }

        for (int row = AtariScreenRows - shift; row < AtariScreenRows; ++row) {
            if (!screenRowsEqual(previous, row, current, row)) {
                hypothesis.exposedRowsChanged = true;
                break;
            }
        }

        const int minimumMatches = hypothesis.overlap <= 2 ? 1 :
                                   hypothesis.overlap <= 4 ? 2 :
                                   (hypothesis.overlap * 2 + 2) / 3;
        const bool shortOverlapHasAnchor = hypothesis.overlap > 4 ||
                                           hypothesis.uniqueAnchors > 0;
        const bool enoughDistinctiveEvidence = hypothesis.overlap <= 4 ?
            hypothesis.uniqueAnchors > 0 :
            ((hypothesis.uniqueAnchors >= 2 &&
              hypothesis.matchingRows * 3 >= hypothesis.overlap * 2) ||
             (hypothesis.uniqueAnchors >= 1 &&
              hypothesis.matchingRows * 10 >= hypothesis.overlap * 9));
        hypothesis.confident = hypothesis.exposedRowsChanged &&
            hypothesis.matchingRows >= minimumMatches &&
            hypothesis.longestMatchRun >= (hypothesis.overlap < 4 ? 1 : 2) &&
            hypothesis.matchingNonblankRows > 0 && shortOverlapHasAnchor &&
            enoughDistinctiveEvidence;
        if (!hypothesis.confident)
            continue;

        const int mismatchedRows = hypothesis.overlap - hypothesis.matchingRows;
        hypothesis.supportScore = hypothesis.uniqueAnchors * 50 +
            hypothesis.longestMatchRun * 10 +
            hypothesis.matchingNonblankRows * 4 +
            hypothesis.matchingRows * 2 - mismatchedRows * 5 - shift;
        if (!bestHypothesis.confident ||
            hypothesis.supportScore > bestHypothesis.supportScore ||
            (hypothesis.supportScore == bestHypothesis.supportScore &&
             hypothesis.overlap > bestHypothesis.overlap)) {
            bestHypothesis = hypothesis;
        }
    }
    return bestHypothesis.shift;
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
        const int overlap = AtariScreenRows - scrollRows;
        for (int row = 0; row < overlap; ++row) {
            if (!screenRowsEqual(previous, row + scrollRows, current, row))
                pendingRows[row] = true;
        }
        for (int row = overlap; row < AtariScreenRows; ++row)
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
