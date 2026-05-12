/* amd64-51 hybrid: replace amd64-51's monolithic ladderstep.S (7196 lines
 * of inlined asm) with a C version that calls fe25519_mul / fe25519_square
 * as separate functions (which our hybrid maps to microcode via the .c
 * wrappers in this directory).
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
 * Note: amd64-51's original ladderstep.S uses a24 = 121666 in a slightly
 * different algebraic formulation. Both are mathematically valid X25519
 * formulas; we use the RFC 7748 one (121665) since fe_mul121665 in our
 * codebase is well-tested. */

#include <stdint.h>
#include <string.h>
#include "fe25519.h"

extern void fe25519_mul(fe25519 *r, const fe25519 *x, const fe25519 *y);
extern void fe25519_square(fe25519 *r, const fe25519 *x);

#define MASK51 0x7FFFFFFFFFFFFULL

static inline void hyb_add(fe25519 *r, const fe25519 *x, const fe25519 *y) {
    r->v[0] = x->v[0] + y->v[0];
    r->v[1] = x->v[1] + y->v[1];
    r->v[2] = x->v[2] + y->v[2];
    r->v[3] = x->v[3] + y->v[3];
    r->v[4] = x->v[4] + y->v[4];
}

static inline void hyb_sub(fe25519 *r, const fe25519 *x, const fe25519 *y) {
    /* Bias by 2*p to keep limbs nonneg, like our existing fe_sub. */
    r->v[0] = (x->v[0] + 0xFFFFFFFFFFFDAULL) - y->v[0];
    r->v[1] = (x->v[1] + 0xFFFFFFFFFFFFEULL) - y->v[1];
    r->v[2] = (x->v[2] + 0xFFFFFFFFFFFFEULL) - y->v[2];
    r->v[3] = (x->v[3] + 0xFFFFFFFFFFFFEULL) - y->v[3];
    r->v[4] = (x->v[4] + 0xFFFFFFFFFFFFEULL) - y->v[4];
}

static inline void hyb_mul121665(fe25519 *r, const fe25519 *x) {
    __uint128_t c;
    c  = (__uint128_t)x->v[0] * 121665;
    r->v[0] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)x->v[1] * 121665;
    r->v[1] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)x->v[2] * 121665;
    r->v[2] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)x->v[3] * 121665;
    r->v[3] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)x->v[4] * 121665;
    r->v[4] = (uint64_t)c & MASK51; c >>= 51;
    r->v[0] += (uint64_t)c * 19;
    uint64_t carry = r->v[0] >> 51;
    r->v[0] &= MASK51;
    r->v[1] += carry;
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
