# ---- keccak Makefile (main implementation) ----
# The Keccak-f[1600] microcode implementation: install the looped 24-round
# permutation patch and KAT-verify it (asm_op_keccak_perm). The patch itself is
# generated + software-verified by keccak_gen.py (run `python3 keccak_gen.py` to
# regenerate keccak_perm.h / keccak_perm_body.h).
#
# The SUPERCOP-measurement apparatus lives in bench/ (its own Makefile); the
# hardware probes live in probes/. This folder builds only the implementation.
#
# Runs from simple/keccak/:  include/ -> ../../../include/ , build/ -> ../../../build/
#
# Build: make PROG=asm_op_keccak_perm
# Run:   sudo taskset -c 0 ./asm_op_keccak_perm_static
CC      := gcc
CFLAGS  := -g -O3 -fPIE -ggdb -Wall -masm=intel -march=x86-64-v2 -mtune=generic
LDFLAGS := -static
LIBS    := ../../../build/libmicro.a

PROG ?= asm_op_keccak_perm

all: $(PROG)_static

# ---- asm_op_keccak_perm: needs -no-pie because LDZX_DSZ64_ASZ32 (only ASZ32
# form exists in opcode.h) only takes 32-bit addresses. With -fPIE, static
# globals can land above 4GB; -no-pie forces them low.
ifeq ($(PROG),asm_op_keccak_perm)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# Build $(PROG)_static from $(PROG).c
$(PROG)_static: $(PROG).c $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(LIBS) -o $@

clean:
	rm -f *_static *.o

.PHONY: all clean
