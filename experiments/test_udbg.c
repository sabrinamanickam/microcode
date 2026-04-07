// test_udbg.c
#include <stdio.h>
#include <stdint.h>

// Provided by lib-micro when you link source/*.c
uint64_t ldat_array_read(uint64_t port, uint64_t array, uint64_t bank, uint64_t word, uint64_t addr);

int main(void) {
    // MS lives behind LDAT port 0x6a0 on GLM/GLP; this will #UD if not unlocked.
    uint64_t v = ldat_array_read(0x6a0, 0, 0, 0, 0);
    printf("read = 0x%lx\n", v);
    return 0;
}

