#include "seqRotate.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s\n", #c); ++fails; } } while (0)

int main() {
    CHECK(mm303StepIndex(0,   0,  1, 64) == 0);   // no rotation
    CHECK(mm303StepIndex(0,   1,  1, 64) == 1);   // +1 -> step 2 first
    CHECK(mm303StepIndex(0,  -1,  1, 64) == 63);  // -1 -> step 64 first
    CHECK(mm303StepIndex(63,  1,  1, 64) == 0);   // wrap forward
    CHECK(mm303StepIndex(0,   1, -1, 64) == 63);  // global invert
    CHECK(mm303StepIndex(0,  64,  1, 64) == 0);   // +64 wraps to 0
    CHECK(mm303StepIndex(5, -63,  1, 64) == 6);   // -63 == +1 (mod 64)
    if (fails) { printf("seq_rotate_test: %d FAIL\n", fails); return 1; }
    printf("seq_rotate_test: OK\n");
    return 0;
}
