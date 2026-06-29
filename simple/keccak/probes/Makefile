# ---- keccak/probes Makefile ----
# One-off hardware probes that established the microcode facts the Keccak
# implementation relies on (segment, intra-triad RAW, signed-8 offset, indexed
# load/store, RSP-as-32nd-register, loop control, patch capacity, I/O cost).
# Split out of ../Makefile on 2026-06-16 so the main keccak/ folder holds only
# the implementation.
#
# Runs from keccak/probes/, two levels below simple/:
#   include/ ->  ../../../../include/   (probe sources use this path)
#   build/   ->  ../../../../build/     (libmicro.a)
# Every probe does memory ops, so it needs its static globals in the low 4GB
# (-no-pie); each gets a per-PROG block below.
#
# Build: make PROG=probe_rsp
# Run:   sudo taskset -c 0 ./probe_rsp_static
CC      := gcc
CFLAGS  := -g -O3 -fPIE -ggdb -Wall -masm=intel -march=x86-64-v2 -mtune=generic
LDFLAGS := -static
LIBS    := ../../../../build/libmicro.a

PROG ?= probe_rsp

all: $(PROG)_static

# ---- every probe needs -no-pie (memory ops -> static globals in low 4GB) ----
ifeq ($(PROG),probe_keccak_io)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_keccak_capacity)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_seg)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_stlf)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_triad)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_loaduse)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_offset)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_loop)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_index)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_rsp)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

ifeq ($(PROG),probe_memops)
CFLAGS := $(filter-out -fPIE,$(CFLAGS)) -no-pie
LDFLAGS := -static -no-pie
endif

$(PROG)_static: $(PROG).c $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(LIBS) -o $@

clean:
	rm -f *_static *.o

.PHONY: all clean
