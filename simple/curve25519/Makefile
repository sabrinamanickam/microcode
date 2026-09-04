# ---- config ----
# Adapted for simple/curve25519/ (gathered from simple/Makefile). Shared trees
# live one level up in simple/ and are reached via ../ — exactly like keccak/:
#   ../supercop-20260330/  ../supercop-data/   (gitignored; restore in simple/)
#   ../../../build/libmicro.a , /home/sabrina/include  (shared microcode infra)
# The CryptOpt asm (cryptopt_mul.asm / cryptopt_sq.asm) is gitignored — drop it
# in THIS folder before running the matrix. Local include/ holds the namespace
# headers; the shared patch.h/ucode_macro.h/misc.h are #included via ../../../.
CC      := gcc
NASM    := nasm
CFLAGS  := -g -O3 -fPIE -ggdb -Wall -masm=intel -march=x86-64-v2 -mtune=generic -I include/
LDFLAGS := -static
LIBS    := ../../../build/libmicro.a

# Default program name (override with: make PROG=foo)
PROG ?= yolo

# ---- default target ----
all: $(PROG)_static

# ---- Match SUPERCOP's compiler flags for full_curve25519 fairness ----
# SUPERCOP tries: gcc -march=native -mtune=native -O{3,2,Os,O} -fwrapv -fPIC -fPIE -gdwarf-4 -Wall
# We use the -O3 variant. Override CFLAGS BEFORE the amd64-51 block so derived
# flag vars (AMD51_CFLAGS) inherit the new value.
# full_curve25519_inline2 is the comprehensive benchmark binary (hosts every
# contender; full_curve25519 is kept on disk but no longer benched) and needs
# the identical flags + external objects.
ifneq (,$(filter full_curve25519 full_curve25519_inline2 bench_attrib bench_fieldop probe_a51ops,$(PROG)))
CFLAGS := -O3 -fwrapv -fPIC -fPIE -gdwarf-4 -Wall -masm=intel -march=native -mtune=native -I include/
endif

# bench_attrib.c #includes full_curve25519_inline2.c as a library (INLINE2_LIB),
# so it needs no EXTRA_OBJS — just libmicro.a. Trick-attribution harness.
# bench_fieldop.c also includes it as a library but additionally links the
# CryptOpt asm to compare field-op backends per-op.

# ---- SUPERCOP amd64-51 X25519 (linked into full_curve25519 only) ----
AMD51_DIR  := ../supercop-20260330/crypto_scalarmult/curve25519/amd64-51
SC_INC     := ../supercop-data/redunlockgbbpce3350c/amd64/include
SC_LIB     := ../supercop-data/redunlockgbbpce3350c/amd64/lib
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
AMD64_DIR  := ../supercop-20260330/crypto_scalarmult/curve25519/amd64-64
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

# ---- The two 5x51 ladder controls (see CONTROLS.md) ----------------------
# amd64-51/ucode uses an inline-asm register-chained ladder whose FE_MUL/FE_SQ
# are assembly-text macros with the microcode fired inline — there is no
# fe25519_mul() call to substitute, so the amd64-64 trick (swap the field-op
# objects under a fixed ladder) cannot be applied to it directly. Instead we
# build the two missing corners of the square on the C ladder:
#
#   amd64-51/asm        qhasm ladderstep.S + qhasm mul/square   (stock)
#   amd64-51/asm-Clad   C ladderstep.c    + qhasm mul/square    <-- AMD51A
#   amd64-51/ucode-Clad C ladderstep.c    + 5x51 microcode      <-- AMD51UC
#   amd64-51/ucode      inline-asm ladder + 5x51 microcode      (stock hybrid)
#
# asm-Clad / asm        = ladder tax, inside amd64-51's own framework
# asmClad / ucodeClad   = field-op effect, ladder AND framework held constant
# ucodeClad / ucode     = ladder coding style, field ops held constant
#
# The C ladderstep lives in extra/amd64-51-ucode/ (it left the main hybrid when
# inline2 took over the ladder); both controls compile that same source.
AMD51CLAD_DIR := extra/amd64-51-ucode

# --- amd64-51/asm-Clad: everything from amd64-51 except the ladder. Native.
AMD51A_CPP  := -I$(AMD51_DIR) -I$(SC_INC) -include include/amd64_51_asmclad_namespace.h
AMD51A_CFLAGS := $(filter-out -masm=intel,$(CFLAGS)) -masm=att
AMD51A_REUSE_C  := mont25519 consts fe25519_invert fe25519_pack fe25519_setint fe25519_unpack
AMD51A_REUSE_S  := fe25519_freeze work_cswap fe25519_mul fe25519_square
AMD51A_NEW_C    := ladderstep

AMD51A_OBJS := \
    $(addprefix amd64-51-asmclad_,$(addsuffix .o,$(AMD51A_REUSE_C))) \
    $(addprefix amd64-51-asmclad_,$(addsuffix .o,$(AMD51A_REUSE_S))) \
    $(addprefix amd64-51-asmclad_,$(addsuffix .o,$(AMD51A_NEW_C)))

define AMD51A_REUSE_C_RULE
amd64-51-asmclad_$(1).o: $$(AMD51_DIR)/$(1).c
	$$(CC) $$(AMD51A_CFLAGS) $$(AMD51A_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD51A_REUSE_C),$(eval $(call AMD51A_REUSE_C_RULE,$(f))))

define AMD51A_REUSE_S_RULE
amd64-51-asmclad_$(1).o: $$(AMD51_DIR)/$(1).S
	$$(CC) $$(AMD51A_CFLAGS) $$(AMD51A_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD51A_REUSE_S),$(eval $(call AMD51A_REUSE_S_RULE,$(f))))

amd64-51-asmclad_ladderstep.o: $(AMD51CLAD_DIR)/ladderstep.c
	$(CC) $(AMD51A_CFLAGS) $(AMD51A_CPP) -c $< -o $@

# --- amd64-51/ucode-Clad: same C ladder, microcode field ops.
AMD51UC_CPP  := -I$(AMD51_DIR) -I$(SC_INC) -include include/amd64_51_ucode_clad_namespace.h
AMD51UC_CFLAGS := $(filter-out -masm=intel,$(CFLAGS)) -masm=att
AMD51UC_REUSE_C  := mont25519 consts fe25519_invert fe25519_pack fe25519_setint fe25519_unpack
AMD51UC_REUSE_S  := fe25519_freeze work_cswap
AMD51UC_WRAP_C   := fe25519_mul fe25519_square

AMD51UC_OBJS := \
    $(addprefix amd64-51-ucodeclad_,$(addsuffix .o,$(AMD51UC_REUSE_C))) \
    $(addprefix amd64-51-ucodeclad_,$(addsuffix .o,$(AMD51UC_REUSE_S))) \
    $(addprefix amd64-51-ucodeclad_,$(addsuffix .o,$(AMD51UC_WRAP_C))) \
    amd64-51-ucodeclad_ladderstep.o

define AMD51UC_REUSE_C_RULE
amd64-51-ucodeclad_$(1).o: $$(AMD51_DIR)/$(1).c
	$$(CC) $$(AMD51UC_CFLAGS) $$(AMD51UC_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD51UC_REUSE_C),$(eval $(call AMD51UC_REUSE_C_RULE,$(f))))

define AMD51UC_REUSE_S_RULE
amd64-51-ucodeclad_$(1).o: $$(AMD51_DIR)/$(1).S
	$$(CC) $$(AMD51UC_CFLAGS) $$(AMD51UC_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD51UC_REUSE_S),$(eval $(call AMD51UC_REUSE_S_RULE,$(f))))

# The microcode-calling wrappers come from the live hybrid dir.
define AMD51UC_WRAP_C_RULE
amd64-51-ucodeclad_$(1).o: $$(AMD51U_DIR)/$(1).c
	$$(CC) $$(AMD51UC_CFLAGS) $$(AMD51UC_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD51UC_WRAP_C),$(eval $(call AMD51UC_WRAP_C_RULE,$(f))))

amd64-51-ucodeclad_ladderstep.o: $(AMD51CLAD_DIR)/ladderstep.c
	$(CC) $(AMD51UC_CFLAGS) $(AMD51UC_CPP) -c $< -o $@

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

# ---- amd64-64/asm-Clad CONTROL: amd64-64 framework + the ucode hybrid's C
#      ladderstep + amd64-64's OWN asm fe25519_mul/square ----
# Isolates the field-op backend from the ladder rewrite. amd64-64/ucode
# replaces ladderstep AND mul/square, so amd64-64/asm vs amd64-64/ucode
# confounds the two. This control differs from amd64-64/ucode in the field
# ops ONLY (same ladderstep.c object source), and from amd64-64/asm in the
# ladder ONLY. Pure native code — installs no microcode patch.
AMD64A_CPP  := -I$(AMD64_DIR) -I$(SC_INC) -include include/amd64_64_asmclad_namespace.h
AMD64A_CFLAGS := $(filter-out -masm=intel,$(CFLAGS)) -masm=att

# Everything from SUPERCOP amd64-64 (recompiled under the control's namespace)
# — note fe25519_mul/fe25519_square are here, NOT replaced.
AMD64A_REUSE_C  := mont25519 consts fe25519_invert fe25519_pack fe25519_setint fe25519_unpack
AMD64A_REUSE_S  := fe25519_freeze work_cswap fe25519_mul fe25519_square
# The one replaced file: the SAME C ladder the ucode hybrid uses.
AMD64A_NEW_C    := ladderstep

AMD64A_OBJS := \
    $(addprefix amd64-64-asmclad_,$(addsuffix .o,$(AMD64A_REUSE_C))) \
    $(addprefix amd64-64-asmclad_,$(addsuffix .o,$(AMD64A_REUSE_S))) \
    $(addprefix amd64-64-asmclad_,$(addsuffix .o,$(AMD64A_NEW_C)))

define AMD64A_REUSE_C_RULE
amd64-64-asmclad_$(1).o: $$(AMD64_DIR)/$(1).c
	$$(CC) $$(AMD64A_CFLAGS) $$(AMD64A_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD64A_REUSE_C),$(eval $(call AMD64A_REUSE_C_RULE,$(f))))

define AMD64A_REUSE_S_RULE
amd64-64-asmclad_$(1).o: $$(AMD64_DIR)/$(1).S
	$$(CC) $$(AMD64A_CFLAGS) $$(AMD64A_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD64A_REUSE_S),$(eval $(call AMD64A_REUSE_S_RULE,$(f))))

# Replaced C file — sourced from amd64-64-ucode/ (identical ladder source).
define AMD64A_NEW_C_RULE
amd64-64-asmclad_$(1).o: $$(AMD64U_DIR)/$(1).c
	$$(CC) $$(AMD64A_CFLAGS) $$(AMD64A_CPP) -c $$< -o $$@
endef
$(foreach f,$(AMD64A_NEW_C),$(eval $(call AMD64A_NEW_C_RULE,$(f))))

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

# full_curve25519_inline2 hosts every contender, so it links the same external
# objects as full_curve25519 (amd64-51 asm, amd64-51-ucode framework, amd64-64
# asm, CryptOpt asm). amd64-64/ucode stays its own binary (4x64 patch can't
# coexist with the 5x51 patches both of these install).
ifeq ($(PROG),full_curve25519)
EXTRA_OBJS := $(AMD51_OBJS) $(AMD51U_OBJS) $(AMD64_OBJS) cryptopt_mul.o cryptopt_sq.o
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
endif

# inline2 defines supercop_amd64_51_ucode_ladderstep itself (inline-asm style),
# so the C ladderstep object is excluded — its other amd64-51-ucode framework
# objects (mont25519/invert/pack/mul/square/...) are still linked.
ifeq ($(PROG),full_curve25519_inline2)
EXTRA_OBJS := $(AMD51_OBJS) \
              $(filter-out amd64-51-ucode_ladderstep.o,$(AMD51U_OBJS)) \
              $(AMD51A_OBJS) $(AMD51UC_OBJS) \
              $(AMD64_OBJS) cryptopt_mul.o cryptopt_sq.o
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
endif

# Standalone amd64-64/ucode hybrid binary: amd64-64 framework + 4x64 microcode
# field ops. Installs the 4x64 patch (can't coexist with full_curve25519's
# 5x51 patches), so it's its own binary — same pattern as inline2.
ifeq ($(PROG),full_curve25519_amd64_64_ucode)
EXTRA_OBJS := $(AMD64U_OBJS)
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
endif

# amd64-64/asm-Clad control binary (pure native; no microcode patch).
# Links the stock amd64-64 objects TOO, so the binary benches amd64-64/asm and
# amd64-64/asm-Clad back-to-back in ONE process — the ladder-rewrite tax comes
# out as a same-process ratio, immune to cross-run frequency state. The two
# object sets use different CRYPTO_NAMESPACE prefixes, so they coexist.
ifeq ($(PROG),full_curve25519_amd64_64_asmclad)
EXTRA_OBJS := $(AMD64A_OBJS) $(AMD64_OBJS)
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
endif

# bench_fieldop: per-op field-op comparison; links CryptOpt asm (fiat is a C
# include, native is inline, microcode comes from the INLINE2_LIB include).
# probe_a51ops includes inline2 as a library to verify/bench the a51ops
# control without installing any patch (no sudo, no red-unlock needed).
ifeq ($(PROG),probe_a51ops)
EXTRA_OBJS := $(AMD51_OBJS) \
              $(filter-out amd64-51-ucode_ladderstep.o,$(AMD51U_OBJS)) \
              $(AMD51A_OBJS) $(AMD51UC_OBJS) \
              $(AMD64_OBJS) cryptopt_mul.o cryptopt_sq.o
EXTRA_LIBS := $(SC_LIB)/libcryptoint.a
endif

ifeq ($(PROG),bench_fieldop)
EXTRA_OBJS := cryptopt_mul.o cryptopt_sq.o
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

# Build $(PROG)_static from $(PROG).c (+ any extra objs/libs)
# The matrix builds: full_curve25519, full_curve25519_inline2,
# full_curve25519_amd64_64_ucode (see lib/build_run.sh).
$(PROG)_static: $(PROG).c $(LIBS) $(EXTRA_OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(EXTRA_OBJS) $(LIBS) $(EXTRA_LIBS) -o $@

# Convenience targets
clean:
	rm -f *_static *.o

.PHONY: all clean
