# ---- Paper_probes Makefile ----
# The hardware probes that establish the constraints stated in the paper's
# methodology section.  Gathered here on 2026-09-10 from simple/, simple/tests/,
# simple/keccak/probes/ and simple/curve25519/ so the evidence for each claim
# lives in one place.  See README.md for the constraint -> probe map.
#
# Runs from simple/Paper_probes/, three levels below the repo root:
#   include/ -> ../../../include/      build/ -> ../../../build/libmicro.a
# -no-pie for every probe: they all use memory operations, whose static globals
# must sit in the low 4GB for the 32 bit address size of LDZX and STAD.
# -I../../../include so that probes using bare #include "patch.h" also build.
#
# Build one:  make PROG=probe_memops
# Build all:  make all
# Run:        sudo taskset -c 0 ./probe_memops_static
CC      := gcc
CFLAGS  := -g -O3 -ggdb -Wall -masm=intel -march=x86-64-v2 -mtune=generic \
           -no-pie -I../../../include
LDFLAGS := -static -no-pie
LIBS    := ../../../build/libmicro.a

# ---------------------------------------------------------------------------
# Probes that live here, by what they establish.  CONSTRAINTS is the set the
# methodology section cites (see the constraint -> probe map in README.md); the
# other four groups are the investigations those constraints were distilled
# from, gathered here on 2026-09-12 from simple/ and tests/.
# ---------------------------------------------------------------------------

# C1-C7, the methodology constraint set.
CONSTRAINTS := probe_memops probe_triad probe_stlf probe_loaduse probe_offset \
         probe_seg probe_loop probe_rsp probe_hookcost probe_keccak_capacity \
         probe_carry probe_opsem probe_shrd probe_rol probe_vmwrite_cost \
         test_raw_war_waw test_slot02_raw test_slot_raw_extended test_mul_slot2 \
         carrystate_scope arch_cf_survival mul_dst_class patch_ram_capacity \
         tmp_persistence probe_trigger_class \
         ujmp_test

# Where the field-op cost actually goes: firing tax against triad count, and
# which half of a field op the microcode edge comes from.
FIELDOP := probe_looped_fieldop probe_sqmul probe_sq_latency probe_interleave \
         probe_loopcost probe_mul_isolate probe_carry_isolate \
         probe_mul_carry_style probe_mul_critpath

# The ADD / SETCC / ADC / GENARITHFLAGS investigation.  additionwithflags is
# the consolidated master test and the one the paper cites; the rest are the
# individual experiments behind it, dead ends included.
CARRY := additionwithflags subtractionwithflags flags simple_ADC simple_ADC_r64 \
         simple_ADC_tmp adcx_probe arch_gfl_chain chain_length \
         chained_4limb_add cr_cf_probe does_add_update_eflags \
         genflagsrr_bridge_verify genflagsrr_exhaustive genflagsrr_final \
         gfl_rr_operand_matrix intra-triad-adc intra-triad-adc-genflagsrr \
         intra-triad-adc-regclass readaflags_probe test_adc \
         test_adc_carry_route test_adc_chain test_adc_dsz64 \
         test_adc_gfl_adc_intratriad test_genarithflags_semantics \
         test_movetocreg_eflags test_setcc_repeated

# The staging buffer: capacity, persistence and per-op cost.
STGBUF := stgbuf_probe stgbuf_bench stgbuf_overlap stgbuf_test

# Patch RAM, the hook mechanism and the micro-op vocabulary.
MECH := probe_patchram multi_capacity_test jump_test dump_slots test_ldzx \
         test_intradata test_2mac_prereqs test_two_patch test_x25519_debug

LOCAL := $(CONSTRAINTS) $(FIELDOP) $(CARRY) $(STGBUF) $(MECH)

# Probes that stay in ../curve25519 because they include
# full_curve25519_inline2.c and link roughly thirty contender objects plus
# SUPERCOP's libcryptoint.a.  These targets delegate to that Makefile so this
# directory remains the single entry point.
DELEGATED := probe_sched probe_issue probe_oplat probe_ldorder probe_trigger \
             probe_wrapper

PROG ?= probe_memops

.PHONY: all local delegated clean list constraints fieldop carry stgbuf mech
all: local

local: $(addsuffix _static,$(LOCAL))

constraints: $(addsuffix _static,$(CONSTRAINTS))
fieldop:     $(addsuffix _static,$(FIELDOP))
carry:       $(addsuffix _static,$(CARRY))
stgbuf:      $(addsuffix _static,$(STGBUF))
mech:        $(addsuffix _static,$(MECH))

delegated:
	@for p in $(DELEGATED); do \
	  echo "=== $$p (in ../curve25519) ==="; \
	  $(MAKE) --no-print-directory -C ../curve25519 PROG=$$p || exit 1; \
	done

%_static: %.c $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(LIBS) -o $@

list:
	@echo "constraints $(CONSTRAINTS)"
	@echo "fieldop     $(FIELDOP)"
	@echo "carry       $(CARRY)"
	@echo "stgbuf      $(STGBUF)"
	@echo "mech        $(MECH)"
	@echo "delegated   $(DELEGATED)"

clean:
	rm -f *_static *.o
