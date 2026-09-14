/*
 * probe_sq_lat.c — why is fe_sq slower than fe_mul in a dependent chain?
 *
 * bench_kernel reports, on the same machine and process:
 *     uc_sq_lat  122.3   uc_sq_tput  84.0     (42-triad patch)
 *     uc_mul_lat 122.8   uc_mul_tput 122.2    (66-triad patch)
 *
 * So fe_mul is ISSUE bound (its 66 triads cost the same whether or not the
 * ops are independent) while fe_sq is LATENCY bound: with ILP exposed it
 * finishes in 84 cyc, but chained it takes 122. The suspected cause is the
 * cross-limb carry chain: limb i+1's accumulator is initialised with
 * OR(TMP0, TMP8, TMP1) — limb i's carry-out — so all five limbs form one
 * serial dependency. fe_mul has the same chain but 24 more triads of
 * independent work to hide it behind; fe_sq does not.
 *
 * Arms:
 *   sq_prod_lat / sq_prod_tput     production patch, chained / independent
 *   sq_cut_lat  / sq_cut_tput      SAME 42 triads, cross-limb carry chain
 *                                  severed (WRONG results — timing probe
 *                                  only). If the theory holds, sq_cut_lat
 *                                  collapses to ~sq_prod_tput.
 *   floor_sq_dep / floor_mul_dep   1-triad patches that DO carry a
 *                                  dependency from an input reg to an output
 *                                  reg, so they measure true dispatch
 *                                  latency (bench_kernel's floor_mul_lat is
 *                                  a throughput number by accident: with a
 *                                  no-op patch the mul wrapper stores the
 *                                  b-operand it just loaded from a fixed
 *                                  slot, so its ping-pong carries no
 *                                  dependency at all).
 *
 * Build: make PROG=probe_sq_lat
 * Run:   sudo taskset -c 0 ./probe_sq_lat_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>

#define K_UNROLL 16
#define K_INNER  8
#define K_REPS   200
#define K_PHASES 4
#define K_OPS    (K_INNER * K_UNROLL)

#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)

static ladder_state_t g_st;

static void init_operands(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

#define UC_SQ_LAT   REP8(FE_SQ(B_OFF, A_OFF)          FE_SQ(A_OFF, B_OFF))
#define UC_MUL_LAT  REP8(FE_MUL(B_OFF, A_OFF, Z2_OFF) FE_MUL(A_OFF, B_OFF, Z2_OFF))
#define UC_SQ_TPUT  REP4(FE_SQ(B_OFF, A_OFF)   FE_SQ(BB_OFF, AA_OFF) \
                         FE_SQ(D_OFF, C_OFF)   FE_SQ(DA_OFF, CB_OFF))

#define MAX_ARMS 32
typedef struct { const char *label; double med; int have; } arm_t;
static arm_t g_arm[MAX_ARMS];
static int   g_narms;
static void arm_record(const char *label, double med) {
    for (int i = 0; i < g_narms; i++)
        if (g_arm[i].label == label) {
            if (med < g_arm[i].med) g_arm[i].med = med;
            return;
        }
    g_arm[g_narms].label = label; g_arm[g_narms].med = med;
    g_arm[g_narms].have = 1; g_narms++;
}
static double arm_get(const char *label) {
    for (int i = 0; i < g_narms; i++)
        if (g_arm[i].label == label) return g_arm[i].med;
    return -1.0;
}

static uint64_t g_smp[K_REPS];

#define TIME_ASM(LABEL, BODY) do {                                            \
    for (int q = 0; q < K_REPS; q++) {                                        \
        register ladder_state_t *_st asm("rbp") = &g_st;                      \
        uint64_t _a = rdtsc_start();                                          \
        for (int _i = 0; _i < K_INNER; _i++)                                  \
            asm volatile(BODY : : "r"(_st)                                    \
                : "rax","rbx","rcx","rdx","rsi","rdi",                        \
                  "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc");\
        g_smp[q] = rdtsc_end() - _a;                                          \
    }                                                                         \
    uint64_t _mn, _md, _p10, _p90;                                            \
    bench_stats(g_smp, K_REPS, &_mn, &_md, &_p10, &_p90);                     \
    arm_record(LABEL, (double)_md / K_OPS);                                   \
} while (0)

/* ════════════════════════════════════════════════════════════════════
 * fe_sq patch, chain-severed variant.
 *
 * Byte-for-byte the production 42-triad patch except:
 *   - the five cross-limb  OR(TMP0, TMP8, TMP1)  carry-ins become
 *     NOTAND(TMP0,TMP0,TMP0) (= 0), so limb i+1 no longer waits on limb i;
 *   - the four  NOTAND(R8,R8,R8)  hi-accumulator resets become
 *     NOTAND(R8,RSI,RSI) — still 0, but sourced from a loop-invariant reg
 *     so the reset does not re-link the limbs through R8.
 * Everything else — every MUL, ADD, SETCC, shift, and the triad packing —
 * is identical, so the issue cost is unchanged and only the dependency
 * structure differs. Results are wrong by construction.
 * ════════════════════════════════════════════════════════════════════ */
#define CARRY_IN_PROD  OR_DSZ64_DRR(TMP0, TMP8, TMP1)
#define CARRY_IN_CUT   NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0)
#define R8_ZERO_PROD   NOTAND_DSZ64_DRR(R8, R8, R8)
#define R8_ZERO_CUT    NOTAND_DSZ64_DRR(R8, RSI, RSI)

#define SQ_PATCH(CIN, R8Z) { \
    /* c0 */ \
    { ZEROEXT_DSZ64_DR(TMP0, RAX), MUL_DSZ64_DRR(RCX, RDI, RDI), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, RDI), SETCC_CONDB_DR(TMP15, TMP0), \
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, RBX, R13), \
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), \
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, RDX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9), \
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), \
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13), \
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD }, \
    { SHR_DSZ64_DRI(RDI, TMP6, 13), ZEROEXT_DSZ64_DR(R13, RSI), \
      R8Z, NOP_SEQWORD }, \
    /* c1 */ \
    { CIN, MUL_DSZ64_DRR(RCX, R15, R13), \
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), \
      ADD_DSZ64_DRR(R9, R12, R12), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, R11, RDX), \
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), \
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, RBX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9), \
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), \
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13), \
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD }, \
    { SHR_DSZ64_DRI(R9, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R12), \
      R8Z, NOP_SEQWORD }, \
    /* c2 */ \
    { CIN, MUL_DSZ64_DRR(RCX, R15, RDX), \
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), \
      ZEROEXT_DSZ64_DR(R13, RSI), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, RSI, R13), \
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), \
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, RBX, R10), ADD_DSZ64_DRR(TMP0, TMP0, R10), \
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), \
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13), \
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD }, \
    { SHR_DSZ64_DRI(R10, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R11), \
      R8Z, NOP_SEQWORD }, \
    /* c3 */ \
    { CIN, MUL_DSZ64_DRR(RCX, R15, RDX), \
      ADD_DSZ64_DRR(R13, RSI, RSI), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), \
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(RAX, R12), MUL_DSZ64_DRR(RCX, R13, RAX), \
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, RAX), SETCC_CONDB_DR(TMP15, TMP0), \
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, R14, RBX), ADD_DSZ64_DRR(TMP0, TMP0, RBX), \
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), \
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(TMP8, TMP0, 51), \
      SHL_DSZ64_DRI(TMP6, TMP0, 13), NOP_SEQWORD }, \
    { SHR_DSZ64_DRI(RBX, TMP6, 13), SHL_DSZ64_DRI(TMP1, R8, 13), \
      R8Z, NOP_SEQWORD }, \
    /* c4 */ \
    { CIN, MUL_DSZ64_DRR(RCX, R15, R14), \
      ADD_DSZ64_DRR(TMP0, TMP0, R14), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), \
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, R13, R11), ADD_DSZ64_DRR(TMP0, TMP0, R11), \
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), \
      MUL_DSZ64_DRR(RCX, R12, R12), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, R12), SETCC_CONDB_DR(TMP15, TMP0), \
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHR_DSZ64_DRI(TMP8, TMP0, 51), \
      ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD }, \
    { SHL_DSZ64_DRI(TMP6, TMP0, 13), SHL_DSZ64_DRI(TMP1, R8, 13), \
      SHR_DSZ64_DRI(RAX, TMP6, 13), NOP_SEQWORD }, \
    /* final reduction */ \
    { CIN, MUL_DSZ64_DIR(TMP6, 19, TMP0), \
      ADD_DSZ64_DRR(RDI, RDI, TMP0), NOP_SEQWORD }, \
    { SHR_DSZ64_DRI(TMP0, RDI, 51), ADD_DSZ64_DRR(R9, R9, TMP0), \
      NOP, NOP_SEQWORD }, \
    { SHL_DSZ64_DRI(TMP6, RDI, 13), SHR_DSZ64_DRI(RDI, TMP6, 13), \
      NOP, END_SEQWORD } \
}

static ucode_t sq_prod[] = SQ_PATCH(CARRY_IN_PROD, R8_ZERO_PROD);
static ucode_t sq_cut[]  = SQ_PATCH(CARRY_IN_CUT,  R8_ZERO_CUT);

/* ════════════════════════════════════════════════════════════════════
 * fe_sq, PROPOSED: carry-in merged LAST instead of FIRST.
 *
 * Production accumulates limb i as   acc = c_i + p0 + p1 + p2, so the
 * incoming carry heads a chain of three dependent ADDs, three SETCCs, the
 * TMP9 fold and the R8 shift before it can produce c_{i+1}: nine dependent
 * ops per limb, five limbs, all serial.
 *
 * Addition is associative, so instead accumulate the three products from
 * ZERO (a chain that starts as soon as the MULs retire and is independent
 * of every earlier limb) and fold the carry in at the END:
 *
 *     (R8:TMP0) = p0 + p1 + p2        <- independent of c_i
 *     TMP0 += c_i ; R8 += carry_out   <- the only cross-limb link
 *     c_{i+1} = (TMP0>>51) | (R8<<13) ;  h_i = TMP0 & (2^51-1)
 *
 * The cross-limb chain drops from nine ops to five (ADD, SETCC, ADD, SHL,
 * OR). The accumulator no longer needs an incoming value, so the initial
 * ADD becomes a ZEROEXT copy and the first SETCC disappears; the hi word
 * is initialised with ZEROEXT(R8,RCX), which also removes the four
 * NOTAND(R8,R8,R8) resets. Those savings pay for the three extra merge
 * ops, so the patch is still 42 triads and the issue cost is unchanged --
 * only the dependency structure differs.
 *
 * Register map is production's:  RDI=a0 RSI=a1 R12=a2 R11=a3 R14=a4
 * R15=2a0 R13=2a1 R9=2a2 R10=2a3 RBX=19a4 RDX=19a3, results in
 * RDI,R9,R10,RBX,RAX. TMP2 is the carry (production leaves it free).
 * ════════════════════════════════════════════════════════════════════ */
static ucode_t sq_fast[] = {
    /* c0 = a0*a0 + 19a4*2a1 + 19a3*2a2   (no carry-in: first limb) */
    { MUL_DSZ64_DRR(RCX, RDI, RDI), ZEROEXT_DSZ64_DR(TMP0, RDI),
      ZEROEXT_DSZ64_DR(R8, RCX), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R13), ADD_DSZ64_DRR(TMP0, TMP0, R13),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ZEROEXT_DSZ64_DR(TMP9, TMP15),
      MUL_DSZ64_DRR(RCX, RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, R9), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15), ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, TMP0, 13), SHL_DSZ64_DRI(TMP1, R8, 13),
      SHR_DSZ64_DRI(RDI, TMP6, 13), NOP_SEQWORD },
    /* operand prep for later limbs, into regs whose old value is consumed */
    { ZEROEXT_DSZ64_DR(R13, RSI), ADD_DSZ64_DRR(R9, R12, R12),
      ZEROEXT_DSZ64_DR(RAX, R12), NOP_SEQWORD },
    /* c1 = 2a0*a1 + a3*19a3 + 19a4*2a2 */
    { OR_DSZ64_DRR(TMP2, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, R13),
      ZEROEXT_DSZ64_DR(TMP0, R13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP2), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, TMP15), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP0, 51), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R12),
      ZEROEXT_DSZ64_DR(R13, RSI), NOP_SEQWORD },
    /* c2 = 2a0*a2 + a1*a1 + 19a4*2a3 */
    { OR_DSZ64_DRR(TMP2, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RDX),
      ZEROEXT_DSZ64_DR(TMP0, RDX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R10), ADD_DSZ64_DRR(TMP0, TMP0, R10),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP2), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, TMP15), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP0, 51), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R11),
      ADD_DSZ64_DRR(R13, RSI, RSI), NOP_SEQWORD },
    /* c3 = 2a0*a3 + 2a1*a2 + a4*19a4 */
    { OR_DSZ64_DRR(TMP2, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RDX),
      ZEROEXT_DSZ64_DR(TMP0, RDX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, R13, RAX),
      ADD_DSZ64_DRR(TMP0, TMP0, RAX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R14, RBX), ADD_DSZ64_DRR(TMP0, TMP0, RBX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP2), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, TMP15), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP0, 51), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP6, 13), NOP, NOP, NOP_SEQWORD },
    /* c4 = 2a0*a4 + 2a1*a3 + a2*a2 */
    { OR_DSZ64_DRR(TMP2, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, R14),
      ZEROEXT_DSZ64_DR(TMP0, R14), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, R13, R11),
      ADD_DSZ64_DRR(TMP0, TMP0, R11), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R12, R12), ADD_DSZ64_DRR(TMP0, TMP0, R12),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP2), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, TMP15), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP0, 51), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RAX, TMP6, 13), NOP, NOP, NOP_SEQWORD },
    /* final reduction: h0 += 19*carry, propagate once into h1 */
    { OR_DSZ64_DRR(TMP2, TMP8, TMP1), MUL_DSZ64_DIR(TMP6, 19, TMP2),
      ADD_DSZ64_DRR(RDI, RDI, TMP2), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51), ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, RDI, 13), SHR_DSZ64_DRI(RDI, TMP6, 13),
      NOP, END_SEQWORD }
};

/* Dependency-carrying 1-triad probes: the result register the wrapper stores
 * is written FROM an input register, so the ping-pong really is a chain. */
static ucode_t dep_floor_sq[]  = { { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD } };
static ucode_t dep_floor_mul[] = { { ZEROEXT_DSZ64_DR(R15, RDI), NOP, NOP, END_SEQWORD } };

#define SQ_ADDR 0x7d08UL      /* 0x7c00 + 66 mul triads * 4 */

static void install_sq(ucode_t *p, int n) {
    patch_ucode(SQ_ADDR, p, n);
    hook_match_and_patch(1, 0x0618, SQ_ADDR);
}

int main(void) {
    if (geteuid() != 0) { printf("needs root (sudo taskset -c 0 ./probe_sq_lat_static)\n"); return 1; }
    printf("=== probe_sq_lat: is fe_sq latency-bound on its cross-limb carry chain? ===\n");
    printf("harness: %d ops/sample, %d samples, %d phases\n\n", K_OPS, K_REPS, K_PHASES);

    assign_to_core(0);
    init_operands();
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    printf("\n");
    if (test_rfc7748()) { printf("RFC 7748 FAILED with production patches - abort\n");
                          init_match_and_patch(); do_fix_IN_patch(); return 1; }
    printf("production patches verified.\n\n");

    /* the local copy must be identical to what install_field_patches wrote */
    install_sq(sq_prod, (int)ARRAY_SZ(sq_prod));
    if (test_rfc7748()) {
        printf("RFC 7748 FAILED with the local sq_prod copy - it is NOT identical\n"
               "to the production patch; the cut variant would be meaningless.\n");
        init_match_and_patch(); do_fix_IN_patch(); return 1;
    }
    printf("local sq_prod copy verified identical (RFC 7748 passes).\n\n");

    printf("sq_prod %d triads, sq_cut %d triads, sq_fast %d triads\n\n",
           (int)ARRAY_SZ(sq_prod), (int)ARRAY_SZ(sq_cut), (int)ARRAY_SZ(sq_fast));

    /* the proposed patch must be CORRECT, not merely fast */
    install_sq(sq_fast, (int)ARRAY_SZ(sq_fast));
    int fast_ok = (test_rfc7748() == 0);
    printf(fast_ok ? "sq_fast: RFC 7748 PASSES - correct.\n\n"
                   : "sq_fast: RFC 7748 FAILS - timing below is for a WRONG patch.\n\n");

    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        install_sq(sq_prod, (int)ARRAY_SZ(sq_prod));
        TIME_ASM("sq_prod_lat",  UC_SQ_LAT);
        TIME_ASM("sq_prod_tput", UC_SQ_TPUT);
        TIME_ASM("mul_prod_lat", UC_MUL_LAT);

        init_operands();
        install_sq(sq_cut, (int)ARRAY_SZ(sq_cut));
        TIME_ASM("sq_cut_lat",  UC_SQ_LAT);
        TIME_ASM("sq_cut_tput", UC_SQ_TPUT);

        init_operands();
        install_sq(sq_fast, (int)ARRAY_SZ(sq_fast));
        TIME_ASM("sq_fast_lat",  UC_SQ_LAT);
        TIME_ASM("sq_fast_tput", UC_SQ_TPUT);

        init_operands();
        install_sq(dep_floor_sq, 1);
        TIME_ASM("floor_sq_dep", UC_SQ_LAT);
    }

    /* dispatch latency at the vmwrite hook: 1-triad patch WITH a dependency */
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        patch_ucode(0x7c00, dep_floor_mul, 1);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        TIME_ASM("floor_mul_dep", UC_MUL_LAT);
    }

    printf("\n  %-34s %8s\n", "arm", "cyc/op");
    printf("  %-34s %8s\n", "----------------------------------", "--------");
    const char *rows[] = { "sq_prod_lat", "sq_prod_tput", "sq_cut_lat", "sq_cut_tput",
                           "sq_fast_lat", "sq_fast_tput",
                           "mul_prod_lat", "floor_sq_dep", "floor_mul_dep", NULL };
    for (int i = 0; rows[i]; i++) printf("  %-34s %8.1f\n", rows[i], arm_get(rows[i]));

    double pl = arm_get("sq_prod_lat"), cl = arm_get("sq_cut_lat"), pt = arm_get("sq_prod_tput");
    printf("\n  cross-limb carry chain costs %.1f cyc of the %.1f-cyc dependent fe_sq\n",
           pl - cl, pl);
    printf("  chain-cut latency vs independent-op throughput: %.1f vs %.1f\n", cl, pt);
    printf("  => an fe_sq whose limbs accumulate independently should land near %.1f\n",
           cl > pt ? cl : pt);
    double fl = arm_get("sq_fast_lat");
    if (fl > 0)
        printf("\n  sq_fast (carry merged last, same 42 triads, %s): %.1f cyc\n"
               "  vs production %.1f  (%+.1f%%)   vs fiat-crypto 117.5  (%+.1f%%)\n",
               fast_ok ? "RFC-verified" : "WRONG RESULTS", fl,
               pl, 100.0 * (fl - pl) / pl, 100.0 * (fl - 117.5) / 117.5);

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
