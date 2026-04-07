# ---- config ----
CC      := gcc
NASM    := nasm
CFLAGS  := -g -O3 -fPIE -ggdb -Wall -masm=intel -march=x86-64-v2 -mtune=generic -I include/
LDFLAGS := -static
LIBS    := ../../build/libmicro.a

# Default program name (override with: make PROG=foo)
PROG ?= yolo

# Check if carry_square_mac128.asm exists -> link its object
ASM_SRC := $(wildcard carry_square_mac128.asm)
ASM_OBJ := $(ASM_SRC:.asm=.o)

# ---- default target ----
all: $(PROG)_static

# Build $(PROG)_static from $(PROG).c + optional asm object
$(PROG)_static: $(PROG).c $(ASM_OBJ) $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(ASM_OBJ) $(LIBS) -o $@

# Assemble .asm -> .o
%.o: %.asm
	$(NASM) -f elf64 -o $@ $<

clean:
	rm -f *_static *.o

.PHONY: all clean
