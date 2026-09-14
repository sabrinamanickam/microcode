/*
 * test_sq_paths.c — bisect WHICH fe_sq call path kills the machine.
 *
 * test_sq_fire.c established two things and its log survived the crash:
 *   - installing both patches is safe
 *   - fe_sq_ucode() fires correctly five times, matching fiat-crypto on
 *     inputs up to 2^53-1, so THE PATCH COMPUTES CORRECTLY AND IS SAFE
 *     when fired from a standalone asm block
 * It then died inside test_rfc7748(), which is ~12 contenders and a
 * thousand ladder iterations -- far too coarse to learn anything from.
 *
 * What the two crashes do NOT support, despite being my first theory: RCX.
 * FE_MUL loads 2^51-1 into RCX and the ladder then fires `vmread rdx, rcx`
 * with it still there. That sequence has been running all week, through
 * every bench_kernel measurement, with the OLD sq_patch. So a large RCX at
 * the vmread trigger is not what kills it.
 *
 * That leaves: the new sq_patch is fine alone but fatal in the ladder. This
 * walks every distinct fe_sq call path from simplest to most complex,
 * fsyncing a stage line around each, so one boot names the path. Each step
 * is also checked against fiat-crypto, because a path that returns WRONG
 * answers before the fatal one is just as much of a clue.
 *
 * TWO PHASES, because a diagnostic that is guaranteed to crash is a poor
 * diagnostic. Phase A fires only the two things the ladder does that
 * fe_sq_ucode does not, and then STOPS:
 *
 *   - fe_mul and fe_sq fired in ONE asm block
 *   - fe_sq_ucode_n, the only path that fires a patch in a LOOP, with its
 *     counter held on the stack across the firing
 *
 * If phase A survives, nothing has crashed and both structural suspects are
 * cleared -- which is worth a boot on its own. If it dies, the log names
 * which one. Only phase B (--full) runs ladder_step, fe_invert_ucode and a
 * whole x25519, and phase B is very likely to crash, since test_rfc7748
 * already did. Do not run it until phase A has been read.
 *
 * Build: make PROG=test_sq_paths
 * Run:   sudo taskset -c 0 ./test_sq_paths_static          (phase A only)
 *        sudo taskset -c 0 ./test_sq_paths_static --full   (expect a crash)
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

static int g_log = -1;
static void stage(const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (g_log >= 0) { (void)!write(g_log, buf, n); fsync(g_log); }
    (void)!write(1, buf, n);
}

static ladder_state_t g_st;
static int g_bad;

static void seed(uint64_t v) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++) p[i] = (v * (i + 7)) & MASK51;
}
static void check(const char *what, const uint64_t *in, const uint64_t *got) {
    uint64_t want[5], ra[5], rb[5];
    fe_sq_fiat(in, want);
    fe_reduce(ra, got); fe_reduce(rb, want);
    if (memcmp(ra, rb, 40)) { g_bad++; stage("      %s: WRONG vs fiat", what); }
    else                      stage("      %s: matches fiat", what);
}

int main(int argc, char **argv) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    int full = (argc > 1 && !strcmp(argv[1], "--full"));
    g_log = open("test_sq_paths.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    /* Two hard resets already. Get the page cache onto the disk before
     * firing anything, so a third does not take the filesystem with it. */
    sync();

    stage("00 helpers (phase %s)", full ? "A+B, B WILL LIKELY CRASH" : "A only");
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    stage("01 installing patches");
    install_field_patches();
    stage("02 installed");

    seed(0x123456789ABCDULL);
    uint64_t a_in[5];

    /* ---- path 1: FE_SQ macro, memory -> memory, its own asm block ---- */
    memcpy(a_in, (uint64_t *)&g_st + A_OFF/8, 40);
    stage("03 about to fire FE_SQ (macro, mem->mem, standalone block)");
    {
        register ladder_state_t *st asm("rbp") = &g_st;
        asm volatile(FE_SQ(AA_OFF, A_OFF) : : "r"(st)
            : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11",
              "r12","r13","r14","r15","memory","cc");
    }
    stage("04 FE_SQ survived");
    check("FE_SQ", a_in, (uint64_t *)&g_st + AA_OFF/8);

    /* ---- path 2: FE_SQ_FROM_REGS, inputs already in registers ---- */
    stage("05 about to fire FE_SQ_FROM_REGS");
    {
        register ladder_state_t *st asm("rbp") = &g_st;
        asm volatile(
            "mov rdi, [rbp + " S(A_OFF) " + 0]\n\t"
            "mov rsi, [rbp + " S(A_OFF) " + 8]\n\t"
            "mov r12, [rbp + " S(A_OFF) " + 16]\n\t"
            "mov r11, [rbp + " S(A_OFF) " + 24]\n\t"
            "mov r14, [rbp + " S(A_OFF) " + 32]\n\t"
            FE_SQ_FROM_REGS(BB_OFF) : : "r"(st)
            : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11",
              "r12","r13","r14","r15","memory","cc");
    }
    stage("06 FE_SQ_FROM_REGS survived");
    check("FE_SQ_FROM_REGS", a_in, (uint64_t *)&g_st + BB_OFF/8);

    /* ---- path 3: fe_mul and fe_sq fired in ONE asm block ---- */
    /* The ladder does this constantly and fe_sq_ucode never does. If the
     * two patches interact -- surviving TMP state, or anything the
     * sequencer carries between firings -- this is where it shows. */
    stage("07 about to fire FE_MUL then FE_SQ_FROM_REGS in one block");
    {
        register ladder_state_t *st asm("rbp") = &g_st;
        asm volatile(
            FE_MUL(C_OFF, A_OFF, B_OFF)
            "mov rdi, [rbp + " S(A_OFF) " + 0]\n\t"
            "mov rsi, [rbp + " S(A_OFF) " + 8]\n\t"
            "mov r12, [rbp + " S(A_OFF) " + 16]\n\t"
            "mov r11, [rbp + " S(A_OFF) " + 24]\n\t"
            "mov r14, [rbp + " S(A_OFF) " + 32]\n\t"
            FE_SQ_FROM_REGS(D_OFF) : : "r"(st)
            : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11",
              "r12","r13","r14","r15","memory","cc");
    }
    stage("08 mixed mul+sq block survived");
    check("sq after mul", a_in, (uint64_t *)&g_st + D_OFF/8);

    /* ---- path 4: fe_sq_ucode_n, the LOOPED firing ---- */
    /* The only fe_sq path with a backward branch and a loop counter held
     * on the stack ACROSS a firing. Nothing else in the tree fires the
     * same patch repeatedly without returning to C in between. */
    for (int n = 1; n <= 5; n++) {
        uint64_t o[5];
        stage("09.%d about to fe_sq_ucode_n(n=%d)", n, n);
        fe_sq_ucode_n(o, a_in, n);
        stage("10.%d fe_sq_ucode_n(n=%d) survived", n, n);
        if (n == 1) check("fe_sq_ucode_n(1)", a_in, o);
    }

    if (!full) {
        init_match_and_patch(); do_fix_IN_patch();
        stage("PHASE A COMPLETE, no crash. Both structural suspects cleared:");
        stage("  mul+sq in one asm block: survived");
        stage("  fe_sq_ucode_n looped firing (n=1..5): survived");
        stage("  %d wrong results", g_bad);
        stage("restored. Re-run with --full only after reading this.");
        if (g_log >= 0) close(g_log);
        return g_bad ? 1 : 0;
    }

    /* ================= PHASE B =================
     * Ordered cheapest and most-likely-informative first. Phase A cleared
     * looped firing at n = 1..5; fe_invert_ucode uses n up to 100, so
     * extend that range before anything larger, because it is the cheapest
     * remaining unknown and fe_invert depends on it. The risk concentrates
     * in the ladder steps at the end. */
    for (int i = 0; i < 4; i++) {
        static const int NS[4] = { 10, 20, 50, 100 };
        uint64_t o[5];
        stage("11.%d about to fe_sq_ucode_n(n=%d)", NS[i], NS[i]);
        fe_sq_ucode_n(o, a_in, NS[i]);
        stage("12.%d fe_sq_ucode_n(n=%d) survived", NS[i], NS[i]);
    }

    stage("13 about to run fe_invert_ucode (uses n = 1,2,5,10,20,50,100)");
    { fe zi; fe_invert_ucode(zi, a_in); (void)zi; }
    stage("14 fe_invert_ucode survived");

    stage("15 about to run ONE ladder_step");
    ladder_step(&g_st);
    stage("16 one ladder_step survived");

    stage("17 about to run 10 ladder_steps");
    for (int i = 0; i < 10; i++) ladder_step(&g_st);
    stage("18 10 ladder_steps survived");

    stage("19 about to run 255 ladder_steps (a full ladder's worth)");
    for (int i = 0; i < 255; i++) ladder_step(&g_st);
    stage("20 255 ladder_steps survived");

    stage("21 about to run one whole x25519_ucode");
    {
        uint8_t sc[32], pt[32], out[32];
        hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc, 32);
        hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", pt, 32);
        x25519_ucode(out, sc, pt);
        stage("22 x25519_ucode survived, out = %02x%02x%02x%02x...",
              out[0], out[1], out[2], out[3]);
        g_bad += (memcmp_hex(out,
            "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32) != 0);
    }

    stage("23 about to run one whole x25519 (the inline ladder)");
    {
        uint8_t sc[32], pt[32], out[32];
        hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc, 32);
        hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", pt, 32);
        x25519(out, sc, pt);
        stage("24 x25519 survived, out = %02x%02x%02x%02x...",
              out[0], out[1], out[2], out[3]);
        g_bad += (memcmp_hex(out,
            "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32) != 0);
    }

    init_match_and_patch(); do_fix_IN_patch();
    stage("25 restored; verdict %s (%d wrong)", g_bad ? "FAIL" : "PASS", g_bad);
    if (g_log >= 0) close(g_log);
    return g_bad ? 1 : 0;
}
