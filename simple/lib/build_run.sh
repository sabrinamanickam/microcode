# lib/build_run.sh — what to build, and the build+bench loop.
#
# Sourced by bench_supercop_matrix.sh. Defines the (compiler, -O) grid and the
# contender list, then run_matrix() which — for every config — rebuilds the
# binaries, runs them under taskset, and feeds each measurement to
# record_result() (see lib/parse.sh).
#
# SUPERCOP picks the fastest result per implementation across all of these flag
# sets, so we sweep the same grid and keep the best per contender.

# Flags every config gets. SUPERCOP's gcc line adds -mtune=native too, but the
# clang line does NOT (clang's -march=native already implies -mtune=native).
# To match exactly, -mtune=native is appended only for gcc in run_matrix below.
SUPERCOP_BASE="-fwrapv -fPIC -fPIE -gdwarf-4 -Wall -march=native -I include/"

# (compiler, -O level) pairs — same -O levels SUPERCOP tries (okcompilers/c).
# We test multiple gcc/clang versions side-by-side; any compiler whose binary
# isn't installed is silently skipped by select_active_configs.
CONFIGS=(
    "gcc-11 -O3"   "gcc-11 -O2"   "gcc-11 -Os"   "gcc-11 -O"
    "gcc-12 -O3"   "gcc-12 -O2"   "gcc-12 -Os"   "gcc-12 -O"
    "gcc-13 -O3"   "gcc-13 -O2"   "gcc-13 -Os"   "gcc-13 -O"
    "clang-14 -O3" "clang-14 -O2" "clang-14 -Os" "clang-14 -O"
    "clang-17 -O3" "clang-17 -O2" "clang-17 -Os" "clang-17 -O"
    "clang-18 -O3" "clang-18 -O2" "clang-18 -Os" "clang-18 -O"
)

# Contender labels printed by the benchmark (must exactly match the start of
# each "label:" printf line in full_curve25519.c). Two contenders are special —
# they come from standalone binaries with their own main() and a bare
# "min:"/"median:" output, built + run separately per config (they can't share
# full_curve25519's binary):
#   - "ours/ucode-inline"  -> full_curve25519_inline2_static
#   - "amd64-64/ucode"     -> full_curve25519_amd64_64_ucode_static
#       (the 4x64 microcode patch can't coexist with the 5x51 patches that
#        full_curve25519 installs, so it lives in its own binary)
CONTENDERS=(
    "ours/hand-C"
    "ours/fiat"
    "ours/cryptopt"
    "donna_c64"
    "amd64-51/asm"
    "amd64-51/ucode"
    "amd64-64/asm"
    "amd64-64/ucode"
    "ours/ucode"
    "ours/ucode-inline"
)

# Configs whose compiler is actually installed. Populated by
# select_active_configs, consumed by run_matrix.
ACTIVE_CONFIGS=()

# select_active_configs — filter CONFIGS down to installed compilers.
# Aborts if nothing usable is found.
select_active_configs() {
    local cfg cc
    for cfg in "${CONFIGS[@]}"; do
        cc="${cfg%% *}"
        if command -v "$cc" >/dev/null 2>&1; then
            ACTIVE_CONFIGS+=("$cfg")
        else
            echo "[skip] $cc not installed — dropping config: $cfg"
        fi
    done

    if [ ${#ACTIVE_CONFIGS[@]} -eq 0 ]; then
        echo "No usable compilers found — abort."
        exit 1
    fi
}

# run_standalone <cfg> <prog> <label> <cc> <cflags> <fail_tag>
#
# Build + run one of the standalone-binary contenders (inline2, amd64-64-ucode)
# that print a bare "min:"/"median:" rather than a "label:" line. Echoes the
# binary's "--- Bench … p90:" section live, then records the result.
run_standalone() {
    local cfg="$1" prog="$2" label="$3" cc="$4" cflags="$5" fail_tag="$6"
    local errlog="${prog}_err.log"

    if make -s PROG="$prog" CC="$cc" CFLAGS="$cflags" >/dev/null 2>"$errlog"; then
        rm -f "$errlog"
        local out mn md
        out=$(sudo taskset -c 0 ./"${prog}_static" 2>&1)
        echo "$out" | sed -n '/--- Bench/,/p90:/p'
        mn=$(echo "$out" | awk '/^min:/    {print $2; exit}')
        md=$(echo "$out" | awk '/^median:/ {print $2; exit}')
        record_result "$cfg" "$label" "$mn" "$md"
    else
        echo "$fail_tag"
        sed 's/^/    /' "$errlog"
        rm -f "$errlog"
    fi
}

# run_matrix — the main sweep. For each active config: clean-rebuild
# full_curve25519, run it, scrape every contender's "label: … min … median …"
# line, then build+run the two standalone-binary contenders. Populates the
# global result tables (via record_result) and the ran_cfgs list.
run_matrix() {
    local cfg cc opt cflags output line label mn md

    for cfg in "${ACTIVE_CONFIGS[@]}"; do
        cc="${cfg%% *}"
        opt="${cfg##* }"

        cflags="$opt $SUPERCOP_BASE -masm=intel"
        if [[ "$cc" == clang* ]]; then
            cflags="$cflags -Qunused-arguments"
        else
            cflags="$cflags -mtune=native"
        fi

        echo
        echo "═══════════════════════════════════════════════════════════════"
        echo "  Config: $cfg"
        echo "  CFLAGS: $cflags"
        echo "═══════════════════════════════════════════════════════════════"

        # Force clean rebuild — .o files cache previous flags.
        rm -f amd64-51_*.o amd64-64_*.o amd64-64-ucode_*.o \
              full_curve25519_static full_curve25519_inline2_static \
              full_curve25519_amd64_64_ucode_static

        if ! make -s PROG=full_curve25519 CC="$cc" CFLAGS="$cflags" >/dev/null 2>build_err.log ; then
            echo "[BUILD FAILED]"
            sed 's/^/    /' build_err.log
            continue
        fi
        rm -f build_err.log

        output=$(sudo taskset -c 0 ./full_curve25519_static 2>&1)

        # Echo the bench section so the user sees per-config numbers in real time.
        echo "$output" | sed -n '/X25519 Benchmark/,/^$/p' | head -20

        ran_cfgs+=("$cfg")

        # Contenders that print a "label: … min N … median M …" line inside
        # full_curve25519's output. The two standalone binaries are handled below.
        for label in "${CONTENDERS[@]}"; do
            [ "$label" = "ours/ucode-inline" ] && continue
            [ "$label" = "amd64-64/ucode" ]    && continue
            line=$(echo "$output" | grep -E "^${label}:" || true)
            [ -z "$line" ] && continue
            mn=$(echo "$line" | grep -oE 'min[[:space:]]+[0-9]+'    | awk '{print $2}')
            md=$(echo "$line" | grep -oE 'median[[:space:]]+[0-9]+' | awk '{print $2}')
            record_result "$cfg" "$label" "$mn" "$md"
        done

        # ── ours/ucode-inline: all-in-one inline-asm 5×51 ladder + ucode field
        #    ops (see project_x25519_4x64_loses / inline_asm_no_help). Different
        #    ladder framing than the other ours/*, so it lives in END_TO_END only.
        run_standalone "$cfg" full_curve25519_inline2 "ours/ucode-inline" \
                       "$cc" "$cflags" "[INLINE BUILD FAILED]"

        # ── amd64-64/ucode: amd64-64 framework (driver, invert, pack/unpack,
        #    cswap) with ladderstep+mul+square swapped for 4×64 chained-ADC
        #    microcode. Same 4×64 saturated representation as amd64-64/asm, so
        #    amd64-64/asm vs amd64-64/ucode is a clean same-ladder field-op
        #    comparison. Separate binary because the 4×64 patch can't coexist
        #    with full_curve25519's 5×51 patches.
        run_standalone "$cfg" full_curve25519_amd64_64_ucode "amd64-64/ucode" \
                       "$cc" "$cflags" "[AMD64-64/UCODE BUILD FAILED]"
    done
}
