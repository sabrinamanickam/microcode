/* amd64-64 hybrid: replace amd64-64's monolithic ladderstep.S (the 4×64
 * saturated qhasm ladder) with a C version that calls fe25519_mul /
 * fe25519_square as separate functions (which our hybrid maps to the 4×64
 * microcode via the .c wrappers in this directory).
 *
 * Standard RFC 7748 Montgomery ladder step using a24 = 121665:
 *
 *   A   = x2 + z2;   AA = A^2
 *   B   = x2 - z2;   BB = B^2
 *   E   = AA - BB
 *   C   = x3 + z3
 *   D   = x3 - z3
 *   DA  = D * A
 *   CB  = C * B
 *   x3' = (DA + CB)^2
 *   z3' = x1 * (DA - CB)^2
 *   x2' = AA * BB
 *   z2' = E * (AA + 121665 * E)
 *
 * Cheap ops (add / sub / mul121665) are native C in 4×64 saturated form,
 * mirroring SUPERCOP amd64-64's lazy-reduction pattern (lifted verbatim
 * from full_curve25519_4x64.c, which passes RFC 7748 §5.2). amd64-64's
 * original ladderstep.S uses a24 = 121666 in a slightly different algebraic
 * formulation; both are mathematically valid X25519 formulas. We use the
 * RFC 7748 one (121665) since our 4×64 fe_mul121665 is well-tested. */

#include <stdint.h>
#include <string.h>
#include "fe25519.h"

extern void fe25519_mul(fe25519 *r, const fe25519 *x, const fe25519 *y);
extern void fe25519_square(fe25519 *r, const fe25519 *x);

/* fe_add — t = x + y, then conditionally fold one (and only one) carry
 * through *38, keeping the result < 2^256 and ≡ x+y (mod p). */
static inline void hyb_add(fe25519 *r, const fe25519 *x, const fe25519 *y) {
    __uint128_t s;
    uint64_t r0, r1, r2, r3, c;
    s = (__uint128_t)x->v[0] + y->v[0];        r0 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)x->v[1] + y->v[1] + c;    r1 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)x->v[2] + y->v[2] + c;    r2 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)x->v[3] + y->v[3] + c;    r3 = (uint64_t)s; c = (uint64_t)(s >> 64);
    uint64_t addt0 = 0, addt1 = c ? 38 : 0;
    s = (__uint128_t)r0 + addt1;               r0 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)r1 + addt0 + c;           r1 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)r2 + addt0 + c;           r2 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)r3 + addt0 + c;           r3 = (uint64_t)s; c = (uint64_t)(s >> 64);
    addt0 = c ? addt1 : 0;
    r0 += addt0;
    r->v[0] = r0; r->v[1] = r1; r->v[2] = r2; r->v[3] = r3;
}

/* fe_sub — t = x - y with a matching borrow-fold via *38. */
static inline void hyb_sub(fe25519 *r, const fe25519 *x, const fe25519 *y) {
    __int128 s;
    uint64_t r0, r1, r2, r3, br;
    s = (__int128)x->v[0] - (__int128)y->v[0];        r0 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)x->v[1] - (__int128)y->v[1] - br;   r1 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)x->v[2] - (__int128)y->v[2] - br;   r2 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)x->v[3] - (__int128)y->v[3] - br;   r3 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    uint64_t subt0 = 0, subt1 = br ? 38 : 0;
    s = (__int128)r0 - subt1;                          r0 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r1 - subt0 - br;                     r1 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r2 - subt0 - br;                     r2 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r3 - subt0 - br;                     r3 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    subt0 = br ? subt1 : 0;
    r0 -= subt0;
    r->v[0] = r0; r->v[1] = r1; r->v[2] = r2; r->v[3] = r3;
}

/* fe_mul121665 — one MUL per limb, then fold the high word back via *38. */
static inline void hyb_mul121665(fe25519 *r, const fe25519 *x) {
    __uint128_t t;
    uint64_t r0, r1, r2, r3, hi;
    t = (__uint128_t)x->v[0] * 121665;                     r0 = (uint64_t)t;
    t = (__uint128_t)x->v[1] * 121665 + (uint64_t)(t >> 64); r1 = (uint64_t)t;
    t = (__uint128_t)x->v[2] * 121665 + (uint64_t)(t >> 64); r2 = (uint64_t)t;
    t = (__uint128_t)x->v[3] * 121665 + (uint64_t)(t >> 64); r3 = (uint64_t)t;
    hi = (uint64_t)(t >> 64);
    uint64_t fold = hi * 38;
    t = (__uint128_t)r0 + fold;                            r0 = (uint64_t)t;
    t = (__uint128_t)r1 + (uint64_t)(t >> 64);             r1 = (uint64_t)t;
    t = (__uint128_t)r2 + (uint64_t)(t >> 64);             r2 = (uint64_t)t;
    r3 += (uint64_t)(t >> 64);
    r->v[0] = r0; r->v[1] = r1; r->v[2] = r2; r->v[3] = r3;
}

void ladderstep(fe25519 *work) {
    /* work[0]=x1, work[1]=x2, work[2]=z2, work[3]=x3, work[4]=z3 */
    fe25519 A, B, AA, BB, E, C, D, DA, CB, t0;

    hyb_add(&A, &work[1], &work[2]);
    fe25519_square(&AA, &A);
    hyb_sub(&B, &work[1], &work[2]);
    fe25519_square(&BB, &B);
    hyb_sub(&E, &AA, &BB);
    hyb_add(&C, &work[3], &work[4]);
    hyb_sub(&D, &work[3], &work[4]);
    fe25519_mul(&DA, &D, &A);
    fe25519_mul(&CB, &C, &B);

    hyb_add(&t0, &DA, &CB);
    fe25519_square(&work[3], &t0);

    hyb_sub(&t0, &DA, &CB);
    fe25519_square(&t0, &t0);
    fe25519_mul(&work[4], &work[0], &t0);

    fe25519_mul(&work[1], &AA, &BB);

    hyb_mul121665(&t0, &E);
    hyb_add(&t0, &AA, &t0);
    fe25519_mul(&work[2], &E, &t0);
}
