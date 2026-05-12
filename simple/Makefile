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

ifeq ($(PROG),full_curve25519)
EXTRA_OBJS := $(AMD51_OBJS) $(AMD51U_OBJS)
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
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
