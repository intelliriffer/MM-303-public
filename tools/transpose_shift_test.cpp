#include "seqTranspose.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s\n", #c); ++fails; } } while (0)

int main() {
    // --- pitch class only (octave disabled) -------------------------------
    CHECK(mm303TransposeShift(12, 12) == 0);   // low C-1, play C
    CHECK(mm303TransposeShift(14, 12) == 2);   // play D
    CHECK(mm303TransposeShift(16, 12) == 4);   // play E
    CHECK(mm303TransposeShift(20, 20) == 0);   // low G#, play G#
    CHECK(mm303TransposeShift(21, 20) == 1);   // play A
    CHECK(mm303TransposeShift(12, 20) == -8);  // play C, below low G#
    CHECK(mm303TransposeShift(64, 12) == 4);   // E3 over low C-1

    // --- octave on: base note 60 (base-octave 5), +-2, low=C1 span 24..48 --
    CHECK(mm303TransposeShift(60, 24, 24, 60, 2) == 0);    // C3 -> no change
    CHECK(mm303TransposeShift(72, 24, 24, 60, 2) == 12);   // C4 -> +1 oct
    CHECK(mm303TransposeShift(84, 24, 24, 60, 2) == 24);   // C5 -> +2 oct
    CHECK(mm303TransposeShift(48, 24, 24, 60, 2) == -12);  // C2 -> -1 oct
    CHECK(mm303TransposeShift(36, 24, 24, 60, 2) == -24);  // C1 -> -2 oct
    CHECK(mm303TransposeShift(64, 24, 24, 60, 2) == 4);    // E3 -> key only
    CHECK(mm303TransposeShift(76, 24, 24, 60, 2) == 16);   // E4 -> +1 oct + key

    // --- auto-shrink: +1 oct would push the pattern past 127 --------------
    CHECK(mm303TransposeShift(72, 96, 20, 60, 2) == 0);    // 96+20+12=128 -> shrunk

    // --- auto-shrink down: -2 oct would drop the low note below 0 ---------
    CHECK(mm303TransposeShift(36, 12, 48, 60, 2) == -12);  // 12-24<0 -> only -1 oct

    // --- base note 48 (base-octave 4) shifts the "no change" zone ---------
    CHECK(mm303TransposeShift(48, 24, 24, 48, 2) == 0);    // C2 -> no change
    CHECK(mm303TransposeShift(60, 24, 24, 48, 2) == 12);   // C3 -> +1 oct

    if (fails) { printf("transpose_shift_test: %d FAIL\n", fails); return 1; }
    printf("transpose_shift_test: OK\n");
    return 0;
}
