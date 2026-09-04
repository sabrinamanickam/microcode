# Curve25519 — microcode vs. standard libraries (SUPERCOP matrix)

The full X25519 head-to-head benchmark: our microcode field arithmetic against
the fastest hand-tuned and compiler-generated scalar implementations, measured
the way SUPERCOP measures (best result per contender across a compiler × `-O`
grid). Gathered into this folder on the same pattern as `../keccak/`: the
curve25519-specific files live here; the large shared trees stay in `simple/`
and are reached via `../`.

## Layout

```
curve25519/
├── bench_supercop_matrix.sh      # orchestrator: freq guard → sweep → RESULTS.md
├── lib/                          # freq_guard / build_run / parse / print_matrix
├── Makefile                      # adapted from simple/Makefile (paths fixed)
├── full_curve25519.c             # main harness: ours/{hand-C,fiat,cryptopt,ucode},
│                                 #   donna_c64, amd64-51/{asm,ucode}, amd64-64/asm
├── full_curve25519_inline2.c     # standalone: ours/ucode-inline
├── full_curve25519_amd64_64_ucode.c  # standalone: amd64-64/ucode (4×64 patch)
├── amd64-51-ucode/               # hybrid: amd64-51 framework + ucode field ops
├── amd64-64-ucode/               # hybrid: amd64-64 framework + ucode field ops
└── include/                      # namespace headers + crypto_scalarmult.h
```

## External dependencies (NOT in this folder — gitignored / shared)

These are reached by relative path and must be present for the full matrix to
build. The microcode-only contenders (`ours/ucode*`) build without them.

| Dependency | Path used | Notes |
|---|---|---|
| Shared microcode infra | `/home/sabrina/include`, `../../../build/libmicro.a` | via `../../../include`, `../../../build` |
| Hand-C field-op backend | `../../curvesC/curve25519_{mul,square}.c` | `ours/hand-C` |
| SUPERCOP source tree | `../supercop-20260330/` | donna_c64, amd64-51, amd64-64 sources (**gitignored**) |
| SUPERCOP data/libs | `../supercop-data/redunlockgbbpce3350c/amd64/{include,lib}` | `libcryptoint.a` (**gitignored**) |
| CryptOpt asm | `cryptopt_mul.asm`, `cryptopt_sq.asm` (this folder) | `ours/cryptopt` (**gitignored — drop in here**) |

If `../supercop-20260330/`, `../supercop-data/`, and the CryptOpt `.asm` files
are not restored, the SUPERCOP-dependent contenders fail to build and are simply
skipped; the rest of the matrix still runs.

## Run

```sh
# pin the core to base frequency first (RDTSC counts at the TSC rate, not core
# clock — see lib/freq_guard.sh):
sudo cpupower frequency-set -g userspace && sudo cpupower frequency-set -f 1.10GHz

# full compiler × -O sweep, best per contender → RESULTS.md
./bench_supercop_matrix.sh

# or one config by hand:
make PROG=full_curve25519 CC=gcc-13 CFLAGS="-Os -fwrapv -fPIC -fPIE -march=native -mtune=native -masm=intel -I include/"
sudo taskset -c 0 ./full_curve25519_static
```

## Notes
- `full_curve25519_inline2.c` currently fails to compile against the shared
  `include/inst.h` (`_MUL_DSZ64` undeclared) — this is a pre-existing source
  issue in the original `simple/` copy, not specific to this folder.
- The 4×64 microcode patch can't coexist with the 5×51 patches
  `full_curve25519` installs, so `amd64-64/ucode` and `ours/ucode-inline` are
  built as their own standalone binaries (see `lib/build_run.sh`).
