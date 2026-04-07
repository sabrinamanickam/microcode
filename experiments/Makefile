# ---- config ----
CC      := gcc
CFLAGS  := -g -O0 -fPIE -ggdb -Wall -masm=intel -march=x86-64-v2 -mtune=generic -I include/
LDFLAGS := -static
LIBS    := ../../build/libmicro.a

# Default program name (override with: make PROG=foo)
PROG ?= yolo

# ---- default target ----
all: $(PROG)_static

# Build $(PROG)_static from $(PROG).c
$(PROG)_static: $(PROG).c $(LIBS)
	$(CC) $(LDFLAGS) $(CFLAGS) $< $(LIBS) -o $@

# Convenience targets
clean:
	rm -f *_static *.o

.PHONY: all clean

