# ---- config ----
CC      := gcc
NASM    := nasm
CFLAGS  := -g -O3 -fPIE -ggdb -Wall -masm=intel -march=x86-64-v2 -mtune=generic -I include/
LDFLAGS := -static
LIBS    := ../../build/libmicro.a

# Default program name (override with: make PROG=foo)
PROG ?= yolo

# ---- default target ----
all: $(PROG)_static

# ---- Match SUPERCOP's compiler flags for full_curve25519 fairness ----
# SUPERCOP tries: gcc -march=native -mtune=native -O{3,2,Os,O} -fwrapv -fPIC -fPIE -gdwarf-4 -Wall
# We use the -O3 variant. Override CFLAGS BEFORE the amd64-51 block so derived
# flag vars (AMD51_CFLAGS) inherit the new value.
ifeq ($(PROG),full_curve25519)
CFLAGS := -O3 -fwrapv -fPIC -fPIE -gdwarf-4 -Wall -masm=intel -march=native -mtune=native -I include/
endif

# ---- SUPERCOP amd64-51 X25519 (linked into full_curve25519 only) ----
AMD51_DIR  := supercop-20260330/crypto_scalarmult/curve25519/amd64-51
SC_INC     := supercop-data/redunlockgbbpce3350c/amd64/include
SC_LIB     := supercop-data/redunlockgbbpce3350c/amd64/lib
AMD51_CPP  := -I$(AMD51_DIR) -I$(SC_INC) -include include/amd64_51_namespace.h
# amd64-51 + supercop crypto_int64.h use AT&T inline asm; override -masm=intel.
AMD51_CFLAGS := $(filter-out -masm=intel,$(CFLAGS)) -masm=att

AMD51_OBJS := \
    amd64-51_mont25519.o \
    amd64-51_consts.o \
    amd64-51_fe25519_invert.o \
    amd64-51_fe25519_pack.o \
    amd64-51_fe25519_setint.o \
    amd64-51_fe25519_unpack.o \
    amd64-51_fe25519_freeze.o \
    amd64-51_fe25519_mul.o \
    amd64-51_fe25519_square.o \
    amd64-51_ladderstep.o \
    amd64-51_work_cswap.o

amd64-51_%.o: $(AMD51_DIR)/%.c
	$(CC) $(AMD51_CFLAGS) $(AMD51_CPP) -c $< -o $@

amd64-51_%.o: $(AMD51_DIR)/%.S
	$(CC) $(AMD51_CFLAGS) $(AMD51_CPP) -c $< -o $@

# ---- SUPERCOP amd64-64 X25519 (linked into full_curve25519 only) ----
# Same Bernstein/Schwabe authors as amd64-51 but uses 4x64-bit saturated
# limbs (radix 2^64) instead of 5x51 unsaturated. lib25519's autotuner
# selects this on Goldmont (~22% faster than amd64-51 there).
AMD64_DIR  := supercop-20260330/crypto_scalarmult/curve25519/amd64-64
AMD64_CPP  := -I$(AMD64_DIR) -I$(SC_INC) -include include/amd64_64_namespace.h
AMD64_CFLAGS := $(filter-out -masm=intel,$(CFLAGS)) -masm=att

AMD64_OBJS := \
    amd64-64_mont25519.o \
    amd64-64_consts.o \
    amd64-64_fe25519_invert.o \
    amd64-64_fe25519_pack.o \
    amd64-64_fe25519_setint.o \
    amd64-64_fe25519_unpack.o \
    amd64-64_fe25519_freeze.o \
    amd64-64_fe25519_mul.o \
    amd64-64_fe25519_square.o \
    amd64-64_ladderstep.o \
    amd64-64_work_cswap.o

amd64-64_%.o: $(AMD64_DIR)/%.c
	$(CC) $(AMD64_CFLAGS) $(AMD64_CPP) -c $< -o $@

amd64-64_%.o: $(AMD64_DIR)/%.S
	$(CC) $(AMD64_CFLAGS) $(AMD64_CPP) -c $< -o $@

# ---- amd64-51-UCODE hybrid: same driver/invert/pack, but ladderstep + mul
#                            + square replaced with microcode-calling C ----
AMD51U_DIR  := amd64-51-ucode
AMD51U_CPP  := -I$(AMD51_DIR) -I$(SC_INC) -include include/amd64_51_ucode_namespace.h
AMD51U_CFLAGS := $(filter-out -masm=intel,$(CFLAGS)) -masm=att

# Reuse from SUPERCOP amd64-51 (recompiled with the hybrid's namespace).
AMD51U_REUSE_C  := mont25519 consts fe25519_invert fe25519_pack fe25519_setint fe25519_unpack
AMD51U_REUSE_S  := fe25519_freeze work_cswap
# New files from amd64-51-ucode/.
AMD51U_NEW_C    := ladderstep fe25519_mul fe25519_square

AMD51U_OBJS := \
    $(addprefix amd64-51-ucode_,$(addsuffix .o,$(AMD51U_REUSE_C))) \
    $(addprefix amd64-51-ucode_,$(addsuffix .o,$(AMD51U_REUSE_S))) \
    $(addprefix amd64-51-ucode_,$(addsuffix .o,$(AMD51U_NEW_C)))

# Reused C files (sourced from AMD51_DIR) — explicit rules per object.
define AMD51U_REUSE_C_RULE
amd64-51-ucode_$(1).o: $$(AMD51_DIR)/$(1).c
	$$(CC) $$(AMD51U_CFLAGS) $$(AMD51U_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD51U_REUSE_C),$(eval $(call AMD51U_REUSE_C_RULE,$(f))))

# Reused .S files (sourced from AMD51_DIR).
define AMD51U_REUSE_S_RULE
amd64-51-ucode_$(1).o: $$(AMD51_DIR)/$(1).S
	$$(CC) $$(AMD51U_CFLAGS) $$(AMD51U_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD51U_REUSE_S),$(eval $(call AMD51U_REUSE_S_RULE,$(f))))

# New C files (sourced from amd64-51-ucode/).
define AMD51U_NEW_C_RULE
amd64-51-ucode_$(1).o: $$(AMD51U_DIR)/$(1).c
	$$(CC) $$(AMD51U_CFLAGS) $$(AMD51U_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD51U_NEW_C),$(eval $(call AMD51U_NEW_C_RULE,$(f))))

# ---- amd64-64-UCODE hybrid: same driver/invert/pack as amd64-64, but
#                            ladderstep + mul + square replaced with C that
#                            calls the 4x64 microcode field ops ----
# amd64-64 is 4x64 saturated (radix 2^64), exactly the microcode's layout,
# so this is a true drop-in (no representation conversion). Built only into
# the standalone full_curve25519_amd64_64_ucode binary.
AMD64U_DIR  := amd64-64-ucode
AMD64U_CPP  := -I$(AMD64_DIR) -I$(SC_INC) -include include/amd64_64_ucode_namespace.h
AMD64U_CFLAGS := $(filter-out -masm=intel,$(CFLAGS)) -masm=att

# Reuse from SUPERCOP amd64-64 (recompiled with the hybrid's namespace).
AMD64U_REUSE_C  := mont25519 consts fe25519_invert fe25519_pack fe25519_setint fe25519_unpack
AMD64U_REUSE_S  := fe25519_freeze work_cswap
# New files from amd64-64-ucode/.
AMD64U_NEW_C    := ladderstep fe25519_mul fe25519_square

AMD64U_OBJS := \
    $(addprefix amd64-64-ucode_,$(addsuffix .o,$(AMD64U_REUSE_C))) \
    $(addprefix amd64-64-ucode_,$(addsuffix .o,$(AMD64U_REUSE_S))) \
    $(addprefix amd64-64-ucode_,$(addsuffix .o,$(AMD64U_NEW_C)))

# Reused C files (sourced from AMD64_DIR) — explicit rules per object.
define AMD64U_REUSE_C_RULE
amd64-64-ucode_$(1).o: $$(AMD64_DIR)/$(1).c
	$$(CC) $$(AMD64U_CFLAGS) $$(AMD64U_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD64U_REUSE_C),$(eval $(call AMD64U_REUSE_C_RULE,$(f))))

# Reused .S files (sourced from AMD64_DIR).
define AMD64U_REUSE_S_RULE
amd64-64-ucode_$(1).o: $$(AMD64_DIR)/$(1).S
	$$(CC) $$(AMD64U_CFLAGS) $$(AMD64U_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD64U_REUSE_S),$(eval $(call AMD64U_REUSE_S_RULE,$(f))))

# New C files (sourced from amd64-64-ucode/).
define AMD64U_NEW_C_RULE
amd64-64-ucode_$(1).o: $$(AMD64U_DIR)/$(1).c
	$$(CC) $$(AMD64U_CFLAGS) $$(AMD64U_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD64U_NEW_C),$(eval $(call AMD64U_NEW_C_RULE,$(f))))

# ---- CryptOpt fe_mul / fe_sq (Goldmont-tuned, generated by CryptOpt) ----
# The .asm files declare `fiat_curve25519_carry_{mul,square}` which clashes
# with the fiat-crypto C version already linked. Sed-rename to `cryptopt_*`
# at assemble time so both backends coexist as separate benchmark contenders.
# Some `mul [mem]` lines lack explicit size — newer NASM requires `qword`.
# Sed appends qword where needed, in the same pass as the GLOBAL rename.
cryptopt_mul.o: cryptopt_mul.asm
	sed -e 's/fiat_curve25519_carry_mul/cryptopt_carry_mul/g' \
	    -e 's/^mul \[/mul qword [/' $< > $@.tmp.asm
	$(NASM) -f elf64 -o $@ $@.tmp.asm
	rm -f $@.tmp.asm
cryptopt_sq.o: cryptopt_sq.asm
	sed -e 's/fiat_curve25519_carry_square/cryptopt_carry_square/g' \
	    -e 's/^mul \[/mul qword [/' $< > $@.tmp.asm
	$(NASM) -f elf64 -o $@ $@.tmp.asm
	rm -f $@.tmp.asm

ifeq ($(PROG),full_curve25519)
EXTRA_OBJS := $(AMD51_OBJS) $(AMD51U_OBJS) $(AMD64_OBJS) cryptopt_mul.o cryptopt_sq.o
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
endif

# Standalone amd64-64/ucode hybrid binary: amd64-64 framework + 4x64 microcode
# field ops. Installs the 4x64 patch (can't coexist with full_curve25519's
# 5x51 patches), so it's its own binary — same pattern as inline2.
ifeq ($(PROG),full_curve25519_amd64_64_ucode)
EXTRA_OBJS := $(AMD64U_OBJS)
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
endif

# Standalone per-op profiler for SUPERCOP amd64-64 (4x64 saturated). Links the
# same AMD64_OBJS the matrix uses. Pure native (no microcode) — runs without
# sudo. Pass the amd64-64 include flags ($(AMD64_CPP)) in CFLAGS on the command
# line so the profiler's own compile sees fe25519.h + the namespace header.
ifeq ($(PROG),amd64_64_profile)
EXTRA_OBJS := $(AMD64_OBJS)
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
endif

# ---- SUPERCOP Keccak baselines (linked into bench_keccak only) ----
# Both impls define a function named `KeccakPermutation`; objcopy renames
# them to unique names so they can coexist in one binary. `keep-global-symbol`
# hides the other (unused) functions so they don't collide either.
SC_KECCAK_DIR := supercop-20260330/crypto_hash/keccakc1024
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

# ---- asm_op_keccak: needs -no-pie because LDZX_DSZ64_ASZ32 (only ASZ32 form
# exists in opcode.h) only takes 32-bit addresses. With -fPIE, static globals
# can land above 4GB; -no-pie forces them low. Pattern follows mac/Makefile.
ifeq ($(PROG),asm_op_keccak)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),asm_op_keccak_perm)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),asm_op_keccak_scall)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# head-to-head: links ALL fast SUPERCOP scalar Keccak variants; -no-pie for ucode buffer.
ifeq ($(PROG),asm_op_keccak_vs)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
EXTRA_OBJS := keccak_x86_64_asm.o keccak_x86_64_shld.o keccak_opt64lcu24.o keccak_opt64lcu24shld.o
endif

# Same -no-pie requirement for the I/O bisect probe.
ifeq ($(PROG),probe_keccak_io)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# SEG-finder probe — also needs static globals in low 4GB.
ifeq ($(PROG),probe_seg)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# STLF / RAW probe — memory ops, needs -no-pie.
ifeq ($(PROG),probe_stlf)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# Intra-triad dependency probe — memory ops, needs -no-pie.
ifeq ($(PROG),probe_triad)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# load-use latency probe — memory ops, needs -no-pie.
ifeq ($(PROG),probe_loaduse)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# offset-field mapping probe — memory ops, needs -no-pie.
ifeq ($(PROG),probe_offset)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# loop-control + indexed-load probe — memory ops, needs -no-pie.
ifeq ($(PROG),probe_loop)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# index-register LDZX/STAD probe — memory ops, needs -no-pie.
ifeq ($(PROG),probe_index)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# RSP-as-data-register probe — memory ops, needs -no-pie.
ifeq ($(PROG),probe_rsp)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

# Build $(PROG)_static from $(PROG).c (+ any extra objs/libs)
$(PROG)_static: $(PROG).c $(LIBS) $(EXTRA_OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(EXTRA_OBJS) $(LIBS) $(EXTRA_LIBS) -o $@

# ---- bench target (original) ----
bench_static: bench.c carry_square_mac128.o $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) bench.c carry_square_mac128.o $(LIBS) -o $@

carry_square_mac128.o: carry_square_mac128.asm
	$(NASM) -f elf64 -o $@ $<

# ---- bench_sq_compare: CryptOpt vs MAC128 vs reference ----
# Place the Goldmont CryptOpt .asm next to bench_sq_compare.c and set
# CRYPTOPT_ASM to its name (without .asm extension):
CRYPTOPT_ASM ?= seed3884720830881002_ratio10479

bench_sq_compare_static: bench_sq_compare.c $(CRYPTOPT_ASM).o $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) bench_sq_compare.c $(CRYPTOPT_ASM).o $(LIBS) -o $@

$(CRYPTOPT_ASM).o: $(CRYPTOPT_ASM).asm
	$(NASM) -f elf64 -o $@ $<

# ---- bench_sq_mac3: 3-MAC vs 1-MAC batched hook comparison ----
bench_sq_mac3_static: bench_sq_mac3.c $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) bench_sq_mac3.c $(LIBS) -o $@

# Convenience targets
clean:
	rm -f *_static *.o

.PHONY: all clean
