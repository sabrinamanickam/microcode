/*
 * test_x25519_debug.c — isolate which primitive is broken
 *
 * Tests fe_frombytes/fe_tobytes round-trip, fe_mul, fe_invert
 * against known values, WITHOUT any microcode.
 *
 * Build: make PROG=test_x25519_debug
 * Run:   ./test_x25519_debug_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#define MASK51 0x7FFFFFFFFFFFFULL
typedef uint64_t fe[5];

static void fe_frombytes(uint64_t *h, const uint8_t *s) {
    uint64_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
    for (int i = 0; i < 8; i++) w0 |= (uint64_t)s[i]    << (8*i);
    for (int i = 0; i < 8; i++) w1 |= (uint64_t)s[8+i]  << (8*i);
    for (int i = 0; i < 8; i++) w2 |= (uint64_t)s[16+i] << (8*i);
    for (int i = 0; i < 8; i++) w3 |= (uint64_t)s[24+i] << (8*i);
    h[0] = w0 & MASK51;
    h[1] = ((w0 >> 51) | (w1 << 13)) & MASK51;
    h[2] = ((w1 >> 38) | (w2 << 26)) & MASK51;
    h[3] = ((w2 >> 25) | (w3 << 39)) & MASK51;
    h[4] = (w3 >> 12) & MASK51;
}

static void fe_reduce(uint64_t *h) {
    uint64_t carry;
    for (int i = 0; i < 4; i++) {
        carry = h[i] >> 51; h[i] &= MASK51;
        h[i+1] += carry;
    }
    carry = h[4] >> 51; h[4] &= MASK51;
    h[0] += carry * 19;
    carry = h[0] >> 51; h[0] &= MASK51;
    h[1] += carry;
}

static void fe_tobytes(uint8_t *s, const uint64_t *h) {
    uint64_t t[5];
    memcpy(t, h, 40);
    fe_reduce(t);
    fe_reduce(t);
    uint64_t m = (t[0] >= 0x7FFFFFFFFFFEDULL) ? 1 : 0;
    for (int i = 1; i < 4; i++)
        m &= (t[i] == MASK51) ? 1 : 0;
    m &= (t[4] >= MASK51) ? 1 : 0;
    t[0] -= m * 0x7FFFFFFFFFFEDULL;
    for (int i = 1; i < 5; i++) t[i] -= m * MASK51;

    uint64_t bits;
    bits = t[0] | (t[1] << 51);
    for (int i = 0; i < 8; i++) s[i] = (bits >> (8*i)) & 0xFF;
    bits = (t[1] >> 13) | (t[2] << 38);
    for (int i = 0; i < 8; i++) s[8+i] = (bits >> (8*i)) & 0xFF;
    bits = (t[2] >> 26) | (t[3] << 25);
    for (int i = 0; i < 8; i++) s[16+i] = (bits >> (8*i)) & 0xFF;
    bits = (t[3] >> 39) | (t[4] << 12);
    for (int i = 0; i < 8; i++) s[24+i] = (bits >> (8*i)) & 0xFF;
}

static void fe_mul(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    uint64_t f0=f[0], f1=f[1], f2=f[2], f3=f[3], f4=f[4];
    uint64_t g0=g[0], g1=g[1], g2=g[2], g3=g[3], g4=g[4];
    uint64_t g1_19=19*g1, g2_19=19*g2, g3_19=19*g3, g4_19=19*g4;

    __uint128_t c0 = (__uint128_t)f0*g0 + (__uint128_t)f1*g4_19
                   + (__uint128_t)f2*g3_19 + (__uint128_t)f3*g2_19
                   + (__uint128_t)f4*g1_19;
    __uint128_t c1 = (__uint128_t)f0*g1 + (__uint128_t)f1*g0
                   + (__uint128_t)f2*g4_19 + (__uint128_t)f3*g3_19
                   + (__uint128_t)f4*g2_19;
    __uint128_t c2 = (__uint128_t)f0*g2 + (__uint128_t)f1*g1
                   + (__uint128_t)f2*g0 + (__uint128_t)f3*g4_19
                   + (__uint128_t)f4*g3_19;
    __uint128_t c3 = (__uint128_t)f0*g3 + (__uint128_t)f1*g2
                   + (__uint128_t)f2*g1 + (__uint128_t)f3*g0
                   + (__uint128_t)f4*g4_19;
    __uint128_t c4 = (__uint128_t)f0*g4 + (__uint128_t)f1*g3
                   + (__uint128_t)f2*g2 + (__uint128_t)f3*g1
                   + (__uint128_t)f4*g0;

    uint64_t carry;
    carry = (uint64_t)(c0>>51); h[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1>>51); h[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2>>51); h[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3>>51); h[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4>>51); h[4] = (uint64_t)c4 & MASK51;
    h[0] += carry * 19;
    carry = h[0] >> 51; h[0] &= MASK51;
    h[1] += carry;
}

static void fe_sq(uint64_t *out, const uint64_t *a) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t d0=2*a0, d1=2*a1, d2=2*a2, d3=2*a3;
    uint64_t r3=19*a3, r4=19*a4;
    __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
    __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
    __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
    __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
    __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;
    uint64_t carry;
    carry = (uint64_t)(c0>>51); out[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1>>51); out[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2>>51); out[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3>>51); out[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4>>51); out[4] = (uint64_t)c4 & MASK51;
    out[0] += carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += carry;
}

static void printhex(const char *label, const uint8_t *s, int n) {
    printf("  %s: ", label);
    for (int i = 0; i < n; i++) printf("%02x", s[i]);
    printf("\n");
}

static void printfe(const char *label, const uint64_t *h) {
    printf("  %s: [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 "]\n",
           label, h[0], h[1], h[2], h[3], h[4]);
}

int main(void) {
    printf("=== X25519 primitive debug ===\n\n");

    /* Test 1: fe_frombytes → fe_tobytes round-trip */
    printf("--- Test 1: frombytes/tobytes round-trip ---\n");
    {
        uint8_t input[32] = {
            0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
            0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
            0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
            0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c
        };
        uint64_t h[5];
        uint8_t output[32];

        fe_frombytes(h, input);
        printfe("fe", h);
        fe_tobytes(output, h);
        printhex("in ", input, 32);
        printhex("out", output, 32);
        printf("  Round-trip: %s\n\n", memcmp(input, output, 32) == 0 ? "PASS" : "FAIL");
    }

    /* Test 2: fe_mul identity: a * 1 == a */
    printf("--- Test 2: fe_mul(a, 1) == a ---\n");
    {
        uint64_t a[5] = {12345, 67890, 11111, 22222, 33333};
        uint64_t one[5] = {1, 0, 0, 0, 0};
        uint64_t result[5];
        fe_mul(result, a, one);
        fe_reduce(result);
        fe_reduce(result);
        printfe("a     ", a);
        printfe("a * 1 ", result);
        printf("  Match: %s\n\n", memcmp(a, result, 40) == 0 ? "PASS" : "FAIL");
    }

    /* Test 3: fe_sq(1) == 1 */
    printf("--- Test 3: fe_sq(1) == 1 ---\n");
    {
        uint64_t one[5] = {1, 0, 0, 0, 0};
        uint64_t result[5];
        fe_sq(result, one);
        fe_reduce(result);
        printfe("1^2", result);
        printf("  Match: %s\n\n", result[0]==1 && result[1]==0 && result[2]==0 && result[3]==0 && result[4]==0 ? "PASS" : "FAIL");
    }

    /* Test 4: fe_invert — z * z^(-1) == 1 */
    printf("--- Test 4: z * z^(-1) == 1 ---\n");
    {
        uint64_t z[5] = {7, 0, 0, 0, 0};
        uint64_t zinv[5], t[5], one[5];

        /* Inversion via repeated squaring (Fermat) */
        uint64_t z2[5], z9[5], z11[5];
        uint64_t z_5_0[5], z_10_0[5], z_20_0[5], z_40_0[5];
        uint64_t z_50_0[5], z_100_0[5];

        fe_sq(z2, z);
        fe_sq(t, z2); fe_sq(t, t);
        fe_mul(z9, t, z);
        fe_mul(z11, z9, z2);
        fe_sq(t, z11);
        fe_mul(z_5_0, t, z9);
        fe_sq(t, z_5_0);
        for (int i = 0; i < 4; i++) fe_sq(t, t);
        fe_mul(z_10_0, t, z_5_0);
        fe_sq(t, z_10_0);
        for (int i = 0; i < 9; i++) fe_sq(t, t);
        fe_mul(z_20_0, t, z_10_0);
        fe_sq(t, z_20_0);
        for (int i = 0; i < 19; i++) fe_sq(t, t);
        fe_mul(z_40_0, t, z_20_0);
        fe_sq(t, z_40_0);
        for (int i = 0; i < 9; i++) fe_sq(t, t);
        fe_mul(z_50_0, t, z_10_0);
        fe_sq(t, z_50_0);
        for (int i = 0; i < 49; i++) fe_sq(t, t);
        fe_mul(z_100_0, t, z_50_0);
        fe_sq(t, z_100_0);
        for (int i = 0; i < 99; i++) fe_sq(t, t);
        fe_mul(t, t, z_100_0);
        for (int i = 0; i < 50; i++) fe_sq(t, t);
        fe_mul(t, t, z_50_0);
        for (int i = 0; i < 5; i++) fe_sq(t, t);
        fe_mul(zinv, t, z11);

        fe_mul(one, z, zinv);
        fe_reduce(one);
        fe_reduce(one);
        printfe("z      ", z);
        printfe("z^(-1) ", zinv);
        printfe("z*z^-1 ", one);
        printf("  Is 1: %s\n\n", one[0]==1 && one[1]==0 && one[2]==0 && one[3]==0 && one[4]==0 ? "PASS" : "FAIL");
    }

    /* Test 5: X25519 with basepoint 9 */
    printf("--- Test 5: X25519(scalar, 9) ---\n");
    {
        /* RFC 7748 section 5.2 test vector 1 */
        uint8_t scalar[32] = {
            0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d,
            0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
            0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
            0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4
        };
        uint8_t point[32] = {
            0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
            0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
            0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
            0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c
        };
        uint8_t expected[32] = {
            0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90,
            0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
            0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
            0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52
        };

        /* Clamp scalar */
        uint8_t e[32];
        memcpy(e, scalar, 32);
        e[0] &= 248;
        e[31] &= 127;
        e[31] |= 64;

        uint64_t u[5];
        fe_frombytes(u, point);

        uint64_t x2[5] = {1,0,0,0,0};
        uint64_t z2[5] = {0,0,0,0,0};
        uint64_t x3[5], z3[5] = {1,0,0,0,0};
        memcpy(x3, u, 40);

        uint64_t swap = 0;
        uint64_t A[5], B[5], C[5], D[5], AA[5], BB[5], E[5], DA[5], CB[5], tt[5];

        for (int pos = 254; pos >= 0; pos--) {
            uint64_t bit = (e[pos / 8] >> (pos & 7)) & 1;
            swap ^= bit;

            /* cswap */
            uint64_t mask = -(uint64_t)(swap & 1);
            for (int i = 0; i < 5; i++) {
                uint64_t x = mask & (x2[i] ^ x3[i]);
                x2[i] ^= x; x3[i] ^= x;
                x = mask & (z2[i] ^ z3[i]);
                z2[i] ^= x; z3[i] ^= x;
            }
            swap = bit;

            for (int i = 0; i < 5; i++) A[i] = x2[i] + z2[i];
            fe_sq(AA, A);
            /* fe_sub inline with 4p bias */
            B[0] = x2[0] + 0x1FFFFFFFFFFFB4ULL - z2[0];
            B[1] = x2[1] + 0x1FFFFFFFFFFFFCULL - z2[1];
            B[2] = x2[2] + 0x1FFFFFFFFFFFFCULL - z2[2];
            B[3] = x2[3] + 0x1FFFFFFFFFFFFCULL - z2[3];
            B[4] = x2[4] + 0x1FFFFFFFFFFFFCULL - z2[4];
            fe_sq(BB, B);

            E[0] = AA[0] + 0x1FFFFFFFFFFFB4ULL - BB[0];
            E[1] = AA[1] + 0x1FFFFFFFFFFFFCULL - BB[1];
            E[2] = AA[2] + 0x1FFFFFFFFFFFFCULL - BB[2];
            E[3] = AA[3] + 0x1FFFFFFFFFFFFCULL - BB[3];
            E[4] = AA[4] + 0x1FFFFFFFFFFFFCULL - BB[4];

            for (int i = 0; i < 5; i++) C[i] = x3[i] + z3[i];
            D[0] = x3[0] + 0x1FFFFFFFFFFFB4ULL - z3[0];
            D[1] = x3[1] + 0x1FFFFFFFFFFFFCULL - z3[1];
            D[2] = x3[2] + 0x1FFFFFFFFFFFFCULL - z3[2];
            D[3] = x3[3] + 0x1FFFFFFFFFFFFCULL - z3[3];
            D[4] = x3[4] + 0x1FFFFFFFFFFFFCULL - z3[4];

            fe_mul(DA, D, A);
            fe_mul(CB, C, B);

            for (int i = 0; i < 5; i++) tt[i] = DA[i] + CB[i];
            fe_sq(x3, tt);

            tt[0] = DA[0] + 0x1FFFFFFFFFFFB4ULL - CB[0];
            tt[1] = DA[1] + 0x1FFFFFFFFFFFFCULL - CB[1];
            tt[2] = DA[2] + 0x1FFFFFFFFFFFFCULL - CB[2];
            tt[3] = DA[3] + 0x1FFFFFFFFFFFFCULL - CB[3];
            tt[4] = DA[4] + 0x1FFFFFFFFFFFFCULL - CB[4];
            fe_sq(tt, tt);
            fe_mul(z3, tt, u);

            fe_mul(x2, AA, BB);

            /* t = 121666 * E */
            {
                __uint128_t c; uint64_t carry;
                c = (__uint128_t)E[0]*121666; tt[0]=(uint64_t)c&MASK51; carry=(uint64_t)(c>>51);
                c = (__uint128_t)E[1]*121666+carry; tt[1]=(uint64_t)c&MASK51; carry=(uint64_t)(c>>51);
                c = (__uint128_t)E[2]*121666+carry; tt[2]=(uint64_t)c&MASK51; carry=(uint64_t)(c>>51);
                c = (__uint128_t)E[3]*121666+carry; tt[3]=(uint64_t)c&MASK51; carry=(uint64_t)(c>>51);
                c = (__uint128_t)E[4]*121666+carry; tt[4]=(uint64_t)c&MASK51; carry=(uint64_t)(c>>51);
                tt[0] += carry * 19;
            }
            for (int i = 0; i < 5; i++) tt[i] = tt[i] + AA[i];
            fe_mul(z2, E, tt);
        }
        /* final cswap */
        {
            uint64_t mask = -(uint64_t)(swap & 1);
            for (int i = 0; i < 5; i++) {
                uint64_t x = mask & (x2[i] ^ x3[i]);
                x2[i] ^= x; x3[i] ^= x;
                x = mask & (z2[i] ^ z3[i]);
                z2[i] ^= x; z3[i] ^= x;
            }
        }

        /* Inversion + final multiply */
        uint64_t recip[5];
        {
            uint64_t z2c[5], z9[5], z11c[5];
            uint64_t z_5_0[5], z_10_0[5], z_20_0[5], z_40_0[5];
            uint64_t z_50_0[5], z_100_0[5], ti[5];
            fe_sq(z2c, z2);
            fe_sq(ti, z2c); fe_sq(ti, ti);
            fe_mul(z9, ti, z2);
            fe_mul(z11c, z9, z2c);
            fe_sq(ti, z11c);
            fe_mul(z_5_0, ti, z9);
            fe_sq(ti, z_5_0);
            for (int i=0;i<4;i++) fe_sq(ti,ti);
            fe_mul(z_10_0, ti, z_5_0);
            fe_sq(ti, z_10_0);
            for (int i=0;i<9;i++) fe_sq(ti,ti);
            fe_mul(z_20_0, ti, z_10_0);
            fe_sq(ti, z_20_0);
            for (int i=0;i<19;i++) fe_sq(ti,ti);
            fe_mul(z_40_0, ti, z_20_0);
            fe_sq(ti, z_40_0);
            for (int i=0;i<9;i++) fe_sq(ti,ti);
            fe_mul(z_50_0, ti, z_10_0);
            fe_sq(ti, z_50_0);
            for (int i=0;i<49;i++) fe_sq(ti,ti);
            fe_mul(z_100_0, ti, z_50_0);
            fe_sq(ti, z_100_0);
            for (int i=0;i<99;i++) fe_sq(ti,ti);
            fe_mul(ti, ti, z_100_0);
            for (int i=0;i<50;i++) fe_sq(ti,ti);
            fe_mul(ti, ti, z_50_0);
            for (int i=0;i<5;i++) fe_sq(ti,ti);
            fe_mul(recip, ti, z11c);
        }
        fe_mul(x2, x2, recip);
        fe_reduce(x2);
        fe_reduce(x2);

        uint8_t out[32];
        fe_tobytes(out, x2);

        printhex("got     ", out, 32);
        printhex("expected", expected, 32);
        printf("  Match: %s\n", memcmp(out, expected, 32) == 0 ? "PASS" : "FAIL");
    }

    return 0;
}

// Add at end of main, before return:
// Actually let me just create a quick test
