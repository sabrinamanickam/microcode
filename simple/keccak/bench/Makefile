# ---- keccak/bench Makefile ----
# SUPERCOP-measurement apparatus for the Keccak microcode implementation:
# the head-to-head harness (asm_op_keccak_vs), the standalone bench
# (bench_keccak), and the SUPERCOP scalar Keccak baselines they link against.
# Split out of ../Makefile on 2026-06-16 so the main keccak/ folder holds only
# the implementation.
#
# This Makefile runs from keccak/bench/, two levels below simple/, so:
#   include/      ->  ../../../../include/   (the harness sources use this path)
#   build/        ->  ../../../../build/     (libmicro.a)
#   SUPERCOP      ->  ../../supercop-20260330/   (lives in simple/)
#   supercop-data ->  ../../supercop-data/       (lives in simple/)
#   generated headers (keccak_perm.h / _body.h) live in the parent keccak/, so
#   asm_op_keccak_vs.c includes them as ../keccak_perm.h.
#
# Build: make PROG=asm_op_keccak_vs
# Run:   sudo taskset -c 0 ./asm_op_keccak_vs_static
# Sweep: sudo ./bench_keccak_matrix.sh
CC      := gcc
CFLAGS  := -g -O3 -fPIE -ggdb -Wall -masm=intel -march=x86-64-v2 -mtune=generic
LDFLAGS := -static
LIBS    := ../../../../build/libmicro.a

# Default to the head-to-head benchmark (microcode vs SUPERCOP scalar variants).
PROG ?= asm_op_keccak_vs

all: $(PROG)_static

# ---- SUPERCOP Keccak baselines (sources stay in simple/supercop-20260330) ----
# Both impls define a function named `KeccakPermutation`; objcopy renames them
# to unique names so they can coexist in one binary. `keep-global-symbol` hides
# the other (unused) functions so they don't collide either.
SC_KECCAK_DIR := ../../supercop-20260330/crypto_hash/keccakc1024
SC_INC        := ../../supercop-data/redunlockgbbpce3350c/amd64/include
KECCAK_CFLAGS := $(filter-out -masm=intel,$(CFLAGS)) -masm=att

keccak_x86_64_asm.o: $(SC_KECCAK_DIR)/x86_64_asm/keccak.s
	$(CC) $(KECCAK_CFLAGS) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakPermutation=keccak_x86_64_asm_perm \
	        $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_x86_64_asm_perm \
	        $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

keccak_opt64lcu6.o: $(SC_KECCAK_DIR)/opt64lcu6/KeccakF-1600-opt64.c
	$(CC) $(KECCAK_CFLAGS) -I$(SC_KECCAK_DIR)/opt64lcu6 -I$(SC_INC) \
	      -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakPermutationOnWords=keccak_opt64lcu6_perm \
	        $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_opt64lcu6_perm \
	        $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

keccak_x86_64_shld.o: $(SC_KECCAK_DIR)/x86_64_shld/keccak.s
	$(CC) $(KECCAK_CFLAGS) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakPermutation=keccak_x86_64_shld_perm $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_x86_64_shld_perm $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

keccak_opt64lcu24.o: $(SC_KECCAK_DIR)/opt64lcu24/KeccakF-1600-opt64.c
	$(CC) $(KECCAK_CFLAGS) -I$(SC_KECCAK_DIR)/opt64lcu24 -I$(SC_INC) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakPermutationOnWords=keccak_opt64lcu24_perm $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_opt64lcu24_perm $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

keccak_opt64lcu24shld.o: $(SC_KECCAK_DIR)/opt64lcu24shld/KeccakF-1600-opt64.c
	$(CC) $(KECCAK_CFLAGS) -I$(SC_KECCAK_DIR)/opt64lcu24shld -I$(SC_INC) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakPermutationOnWords=keccak_opt64lcu24shld_perm $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_opt64lcu24shld_perm $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

ifeq ($(PROG),bench_keccak)
EXTRA_OBJS := keccak_x86_64_asm.o keccak_opt64lcu6.o
endif

# head-to-head: links ALL fast SUPERCOP scalar Keccak variants; -no-pie because
# the harness installs the microcode patch and needs its buffer in the low 4GB
# (LDZX_DSZ64_ASZ32 only takes 32-bit addresses).
ifeq ($(PROG),asm_op_keccak_vs)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
EXTRA_OBJS := keccak_x86_64_asm.o keccak_x86_64_shld.o keccak_opt64lcu24.o keccak_opt64lcu24shld.o
endif

# Build $(PROG)_static from $(PROG).c (+ any extra objs)
$(PROG)_static: $(PROG).c $(LIBS) $(EXTRA_OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(EXTRA_OBJS) $(LIBS) -o $@

clean:
	rm -f *_static *.o

.PHONY: all clean
