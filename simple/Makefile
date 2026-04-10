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

# Build $(PROG)_static from $(PROG).c
$(PROG)_static: $(PROG).c $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(LIBS) -o $@

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
