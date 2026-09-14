/*
 * clear_hooks.c — put the CPU's match-and-patch state back to stock.
 *
 * Every probe and benchmark in this tree restores microcode state on exit,
 * including on its failure paths — but a SIGSEGV or a SIGKILL bypasses all
 * of them, and the match/patch entries live in CPU state, not in the
 * process. So after a crashed run the vmwrite (0x0cd8) and vmread (0x0618)
 * hooks are still pointing at whatever patch RAM the dead process installed,
 * for every process on the machine. Nothing in userspace normally executes
 * those two instructions, but KVM does, so do not leave it that way.
 *
 * This is the recovery tool: it re-runs the match-and-patch init sequence,
 * which clears the hook entries, and re-applies the IN-instruction fix that
 * lib-micro's helpers expect to be in place.
 *
 * Build: make PROG=clear_hooks
 * Run:   sudo taskset -c 0 ./clear_hooks_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include "../../../include/patch.h"
#include "../../../include/ucode_macro.h"
#include "../../../include/misc.h"

int main(void) {
    if (geteuid() != 0) {
        printf("needs root: sudo taskset -c 0 ./clear_hooks_static\n");
        return 1;
    }
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    printf("match-and-patch state reset; vmwrite/vmread hooks cleared.\n");
    return 0;
}
