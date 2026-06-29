# Keccak SUPERCOP-matrix benchmark

Same methodology as `../bench_supercop_matrix.sh`: for each (compiler, -O)
config SUPERCOP tries, rebuild every runnable SUPERCOP Keccak baseline (hand
asm, 64-bit C, x86 SIMD, reference) + the
head-to-head harness, run back-to-back in one process, keep the best per
contender. cyc/perm (RDTSC at TSC rate; pin to base so ticks ≈ true cycles).

Configs that ran: 24 / 24

## min cyc/perm per (config × contender)

| config | asm | shld | opt24 | opt24shld | lcu6 | u6 | sse | mmx | simple | xg64 | xg64lc | ossl | ucode |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 2066 | 10103 | 2151 | 10159 | 2220 | 2395 | 3092 | 3944 | 2370 | 2272 | 2153 | 2048 | 1913 |
| gcc-11 -O2 | 2066 | 10102 | 2151 | 10158 | 2220 | 2395 | 3086 | 3946 | 2371 | 2272 | 2153 | 2048 | 1913 |
| gcc-11 -Os | 2064 | 10103 | 2043 | 10061 | 2222 | 2395 | 3093 | 3942 | 2370 | 2271 | 2152 | 2048 | 1913 |
| gcc-11 -O | 2066 | 10102 | 2119 | 10173 | 2220 | 2395 | 3093 | 3946 | 2370 | 2272 | 2153 | 2047 | 1913 |
| gcc-12 -O3 | 2068 | 10101 | 2256 | 10266 | 2218 | 2395 | 3092 | 3941 | 2370 | 2273 | 2152 | 2048 | 1913 |
| gcc-12 -O2 | 2067 | 10101 | 2255 | 10264 | 2219 | 2395 | 3087 | 3944 | 2370 | 2273 | 2151 | 2048 | 1912 |
| gcc-12 -Os | 2066 | 10102 | 2148 | 10171 | 2218 | 2394 | 3087 | 3945 | 2370 | 2272 | 2153 | 2048 | 1913 |
| gcc-12 -O | 2068 | 10101 | 2236 | 10294 | 2218 | 2395 | 3086 | 3946 | 2370 | 2272 | 2153 | 2048 | 1913 |
| gcc-13 -O3 | 2068 | 10101 | 2230 | 10241 | 2219 | 2395 | 3087 | 3946 | 2370 | 2273 | 2152 | 2048 | 1912 |
| gcc-13 -O2 | 2068 | 10101 | 2230 | 10241 | 2219 | 2395 | 3086 | 3941 | 2370 | 2273 | 2153 | 2048 | 1913 |
| gcc-13 -Os | 2066 | 10101 | 2133 | 10169 | 2218 | 2394 | 3093 | 3944 | 2370 | 2272 | 2152 | 2048 | 1913 |
| gcc-13 -O | 2068 | 10103 | 2219 | 10294 | 2219 | 2394 | 3091 | 3946 | 2370 | 2273 | 2151 | 2047 | 1913 |
| clang-14 -O3 | 2066 | 10102 | 2079 | 10079 | 2218 | 2395 | 3054 | 3948 | 2378 | 2270 | 2153 | 2050 | 1912 |
| clang-14 -O2 | 2065 | 10102 | 2079 | 10079 | 2218 | 2394 | 3053 | 3948 | 2378 | 2269 | 2153 | 2049 | 1912 |
| clang-14 -Os | 2065 | 10102 | 2102 | 10090 | 2218 | 2395 | 3058 | 3948 | 2378 | 2269 | 2153 | 2050 | 1912 |
| clang-14 -O | 2066 | 10102 | 2112 | 10139 | 2218 | 2395 | 3057 | 3948 | 2378 | 2270 | 2154 | 2050 | 1912 |
| clang-17 -O3 | 2065 | 10101 | 2094 | 10065 | 2216 | 2395 | 3048 | 3948 | 2378 | 2269 | 2153 | 2050 | 1912 |
| clang-17 -O2 | 2066 | 10101 | 2094 | 10066 | 2216 | 2395 | 3048 | 3949 | 2378 | 2269 | 2153 | 2049 | 1912 |
| clang-17 -Os | 2065 | 10102 | 2083 | 10058 | 2216 | 2395 | 3052 | 3948 | 2378 | 2269 | 2153 | 2050 | 1912 |
| clang-17 -O | 2065 | 10100 | 2131 | 10126 | 2216 | 2395 | 3048 | 3948 | 2377 | 2269 | 2153 | 2050 | 1912 |
| clang-18 -O3 | 2066 | 10102 | 2088 | 10074 | 2216 | 2395 | 3052 | 3948 | 2378 | 2269 | 2153 | 2050 | 1912 |
| clang-18 -O2 | 2066 | 10100 | 2088 | 10073 | 2216 | 2395 | 3048 | 3948 | 2378 | 2269 | 2153 | 2050 | 1912 |
| clang-18 -Os | 2065 | 10101 | 2087 | 10071 | 2216 | 2395 | 3047 | 3948 | 2377 | 2268 | 2153 | 2050 | 1912 |
| clang-18 -O | 2065 | 10101 | 2138 | 10136 | 2216 | 2395 | 3051 | 3948 | 2378 | 2269 | 2156 | 2049 | 1912 |

## best per contender (what SUPERCOP's autotuner would pick)

`this/microcode` = this_contender_cyc / microcode_cyc; >1 means microcode is faster
(e.g. 1.071x = microcode is ~7% faster; 1.000x is microcode itself).

| contender | best min | best median | winning config | this/microcode |
|---|---|---|---|---|
| keccak/x86_64_asm | 2064 | 2073 | gcc-11 -Os | 1.079x |
| keccak/x86_64_shld | 10100 | 10112 | clang-17 -O | 5.282x |
| keccak/opt64lcu24 | 2043 | 2051 | gcc-11 -Os | 1.069x |
| keccak/opt64lcu24shld | 10058 | 10069 | clang-17 -Os | 5.260x |
| keccak/opt64lcu6 | 2216 | 2218 | clang-17 -O3 | 1.159x |
| keccak/opt64u6 | 2394 | 2396 | gcc-12 -Os | 1.252x |
| keccak/sseu2 | 3047 | 3053 | clang-18 -Os | 1.594x |
| keccak/mmxu1 | 3941 | 3953 | gcc-12 -O3 | 2.061x |
| keccak/simple | 2370 | 2371 | gcc-11 -O3 | 1.240x |
| keccak/xkcp_g64 | 2268 | 2270 | clang-18 -Os | 1.186x |
| keccak/xkcp_g64lc | 2151 | 2160 | gcc-12 -O2 | 1.125x |
| keccak/openssl | 2047 | 2053 | gcc-11 -O | 1.071x |
| keccak/microcode | 1912 | 1917 | gcc-12 -O2 | 1.000x |

## headline

- fastest SUPERCOP baseline: **keccak/opt64lcu24 = 2043 cyc/perm** (gcc-11 -Os)
- microcode (looped): **1912 cyc/perm** (gcc-12 -O2)
- ratio microcode / fastest SUPERCOP: **0.936x** (microcode WINS)
