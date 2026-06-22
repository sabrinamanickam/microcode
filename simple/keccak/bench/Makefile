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

keccak_opt64u6.o: $(SC_KECCAK_DIR)/opt64u6/KeccakF-1600-opt64.c
	$(CC) $(KECCAK_CFLAGS) -I$(SC_KECCAK_DIR)/opt64u6 -I$(SC_INC) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakPermutationOnWords=keccak_opt64u6_perm $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_opt64u6_perm $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

# SIMD variants: SSE2/SSSE3 + MMX are baseline under -march=x86-64-v2 / -march=native
# on this Goldmont core, so no extra -m flags are needed. (xopu24 is omitted: XOP
# is AMD-only and #UDs on Intel.) Same KeccakPermutationOnWords(UINT64*) entry point.
keccak_sseu2.o: $(SC_KECCAK_DIR)/sseu2/KeccakF-1600-opt64.c
	$(CC) $(KECCAK_CFLAGS) -I$(SC_KECCAK_DIR)/sseu2 -I$(SC_INC) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakPermutationOnWords=keccak_sseu2_perm $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_sseu2_perm $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

keccak_mmxu1.o: $(SC_KECCAK_DIR)/mmxu1/KeccakF-1600-opt64.c
	$(CC) $(KECCAK_CFLAGS) -I$(SC_KECCAK_DIR)/mmxu1 -I$(SC_INC) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakPermutationOnWords=keccak_mmxu1_perm $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_mmxu1_perm $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

# reference C. Keccak-simple.c #includes a SUPERCOP-generated crypto_hash.h; since
# we only extract the bare permutation KeccakF, a one-line stub (just
# crypto_hash_BYTES) satisfies the compile. KeccakF(state, in, laneCount) with
# laneCount=0 (see asm_op_keccak_vs.c) is a pure permutation.
keccak_simple.o: $(SC_KECCAK_DIR)/simple/Keccak-simple.c
	mkdir -p scstub
	printf '#define crypto_hash_BYTES 64\nint crypto_hash(unsigned char*,const unsigned char*,unsigned long long);\n' > scstub/crypto_hash.h
	$(CC) $(KECCAK_CFLAGS) -Iscstub -I$(SC_KECCAK_DIR)/simple -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakF=keccak_simple_F $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_simple_F $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

# ---- XKCP (Keccak team) single Keccak-p[1600], plain-64bits ----
# Goldmont has no AVX2/AVX-512, and XKCP ships NO SSE single-permutation kernel,
# so the only XKCP single-perm that runs here is plain-64bits (its generic64 /
# generic64lc targets; SSSE3/AVX/x86-64 all fall back to it for one permutation).
# Same KeccakP-1600-opt64.c, toggled by a generated config.h:
#   xkcp_g64   = full unroll, no lane-complement   (generic64)
#   xkcp_g64lc = full unroll, lane-complement       (generic64lc — what XKCP runs here)
# Entry point KeccakP1600_plain64_Permute_24rounds(void*) is renamed per variant.
XKCP_K1600 := ../../XKCP/lib/low/KeccakP-1600
XKCP_INC   := -I$(XKCP_K1600)/plain-64bits -I$(XKCP_K1600)/plain-64bits/SnP \
              -I$(XKCP_K1600)/common -I../../XKCP/lib/low/common -I../../XKCP/lib/common

keccak_xkcp_g64.o: $(XKCP_K1600)/plain-64bits/KeccakP-1600-opt64.c
	mkdir -p xkcpcfg_g64
	printf '#define KeccakP1600_plain64_fullUnrolling\n' > xkcpcfg_g64/config.h
	$(CC) $(KECCAK_CFLAGS) -Ixkcpcfg_g64 $(XKCP_INC) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakP1600_plain64_Permute_24rounds=keccak_xkcp_g64_perm $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_xkcp_g64_perm $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

keccak_xkcp_g64lc.o: $(XKCP_K1600)/plain-64bits/KeccakP-1600-opt64.c
	mkdir -p xkcpcfg_g64lc
	printf '#define KeccakP1600_plain64_useLaneComplementing\n#define KeccakP1600_plain64_fullUnrolling\n' > xkcpcfg_g64lc/config.h
	$(CC) $(KECCAK_CFLAGS) -Ixkcpcfg_g64lc $(XKCP_INC) -c $< -o $@.tmp.o
	objcopy --redefine-sym KeccakP1600_plain64_Permute_24rounds=keccak_xkcp_g64lc_perm $@.tmp.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_xkcp_g64lc_perm $@.tmp2.o $@
	rm -f $@.tmp.o $@.tmp2.o

# ---- OpenSSL keccak1600, x86-64 assembly (crypto/sha/asm, CRYPTOGAMS/Polyakov) ----
# What OpenSSL actually runs on x86-64: plain scalar asm (no BMI/AVX), so it runs
# on Goldmont. Generated from the perlasm via x86_64-xlate.pl. KeccakF1600 isn't
# .globl in the asm (only SHA3_absorb/squeeze are) -> globalize + rename it. The
# trailing .note.gnu.property CET block is emitted in a Solaris #alloc form GNU as
# rejects, and it sits after all code -> stripped. (The avx2/avx512 OpenSSL keccak
# asm needs AVX, which Goldmont lacks, so only this scalar path is benchmarkable.)
OSSL_KECCAK := ../../openssl/crypto/sha/asm/keccak1600-x86_64.pl
keccak_openssl.o: $(OSSL_KECCAK)
	perl $< elf $@.s 2>/dev/null
	sed -i '/\.section[[:space:]].*note\.gnu\.property/,$$d' $@.s
	$(CC) -c $@.s -o $@.tmp.o
	objcopy --globalize-symbol=KeccakF1600 $@.tmp.o $@.tmp1.o
	objcopy --redefine-sym KeccakF1600=keccak_openssl_perm $@.tmp1.o $@.tmp2.o
	objcopy --keep-global-symbol=keccak_openssl_perm $@.tmp2.o $@
	rm -f $@.s $@.tmp.o $@.tmp1.o $@.tmp2.o

ifeq ($(PROG),bench_keccak)
EXTRA_OBJS := keccak_x86_64_asm.o keccak_opt64lcu6.o
endif

# head-to-head: links ALL fast SUPERCOP scalar Keccak variants; -no-pie because
# the harness installs the microcode patch and needs its buffer in the low 4GB
# (LDZX_DSZ64_ASZ32 only takes 32-bit addresses).
ifeq ($(PROG),asm_op_keccak_vs)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
EXTRA_OBJS := keccak_x86_64_asm.o keccak_x86_64_shld.o \
              keccak_opt64lcu24.o keccak_opt64lcu24shld.o \
              keccak_opt64lcu6.o keccak_opt64u6.o \
              keccak_sseu2.o keccak_mmxu1.o keccak_simple.o \
              keccak_xkcp_g64.o keccak_xkcp_g64lc.o keccak_openssl.o
endif

# Build $(PROG)_static from $(PROG).c (+ any extra objs)
$(PROG)_static: $(PROG).c $(LIBS) $(EXTRA_OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(EXTRA_OBJS) $(LIBS) -o $@

clean:
	rm -f *_static *.o keccak_openssl.o.s
	rm -rf scstub xkcpcfg_g64 xkcpcfg_g64lc

.PHONY: all clean
