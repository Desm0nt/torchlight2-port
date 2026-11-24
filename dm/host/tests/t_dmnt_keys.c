// [TL2] dm_keys_mask (dm_input.h): the dmnt key mask the key-press cheat opcode compares against.
#include <assert.h>
#include <stdio.h>
#include "dm_input.h"
int main(void) {
    assert(dm_keys_mask(0, 0, 0, 0, 0) == 0);
    assert(dm_keys_mask(DM_BTN_A | DM_BTN_B, 0, 0, 0, 0) == 0x3);
    assert(dm_keys_mask(1ull << 16, 0, 0, 0, 0) == 0);
    assert(dm_keys_mask(0, -20000, 0, 0, 0) == 1u << 16);
    assert(dm_keys_mask(0, 0, 20000, 0, 0) == 1u << 17);
    assert(dm_keys_mask(0, 0, 0, 20000, 0) == 1u << 22);
    assert(dm_keys_mask(0, 10000, -10000, 10000, -10000) == 0);
    printf("DMNT_KEYS OK\n");
    return 0;
}
