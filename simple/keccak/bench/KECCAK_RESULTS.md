# Keccak SUPERCOP-matrix benchmark

Same methodology as `../bench_supercop_matrix.sh`: for each (compiler, -O)
config SUPERCOP tries, rebuild every runnable SUPERCOP Keccak baseline (hand
asm, 64-bit C, x86 SIMD, reference) + the
head-to-head harness, run back-to-back in one process, keep the best per
contender. cyc/perm (RDTSC at TSC rate; pin to base so ticks ≈ true cycles).
Headline statistic is the **median**; min and the p10–p90 spread are also reported.

Configs that ran: 23 / 24

Environment: core 1, base-pinned, turbo off. Delivered core freq 1100 MHz,
TSC (RDTSC) rate 1094 MHz, correction f_core/f_TSC = 1.00548
(aperf/mperf under load, verified before the sweep; ratios invariant to it, multiply
absolute cycle counts by it for true core cycles).

## median cyc/perm per (config × contender)

| config | asm | shld | opt24 | opt24shld | lcu6 | u6 | sse | mmx | simple | xg64 | xg64lc | ossl | ucode |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 2075 | 10112 | 2162 | 10169 | 2224 | 2398 | 3096 | 3962 | 2374 | 2276 | 2161 | 2056 | 1896 |
| gcc-11 -O2 | 2076 | 10114 | 2163 | 10174 | 2224 | 2399 | 3101 | 3962 | 2374 | 2276 | 2161 | 2056 | 1896 |
| gcc-11 -Os | 2072 | 10113 | 2055 | 10074 | 2225 | 2399 | 3097 | 3961 | 2374 | 2276 | 2161 | 2056 | 1896 |
| gcc-11 -O | 2076 | 10122 | 2133 | 10196 | 2226 | 2400 | 3098 | 3961 | 2375 | 2279 | 2162 | 2056 | 1896 |
| gcc-12 -O3 | 2076 | 10109 | 2260 | 10279 | 2222 | 2398 | 3096 | 3962 | 2374 | 2276 | 2160 | 2056 | 1896 |
| gcc-12 -O2 | 2076 | 10111 | 2261 | 10279 | 2222 | 2398 | 3096 | 3962 | 2375 | 2276 | 2160 | 2056 | 1896 |
| gcc-12 -Os | 2075 | 10111 | 2160 | 10182 | 2221 | 2398 | 3102 | 3962 | 2374 | 2275 | 2161 | 2055 | 1896 |
| gcc-13 -O3 | 2077 | 10112 | 2236 | 10257 | 2222 | 2398 | 3098 | 3962 | 2374 | 2277 | 2161 | 2056 | 1896 |
| gcc-13 -O2 | 2076 | 10110 | 2235 | 10256 | 2222 | 2398 | 3100 | 3961 | 2373 | 2276 | 2160 | 2055 | 1896 |
| gcc-13 -Os | 2075 | 10111 | 2144 | 10182 | 2222 | 2399 | 3096 | 3962 | 2374 | 2276 | 2161 | 2056 | 1896 |
| gcc-13 -O | 2076 | 10111 | 2225 | 10310 | 2222 | 2398 | 3102 | 3962 | 2374 | 2276 | 2160 | 2056 | 1896 |
| clang-14 -O3 | 2074 | 10124 | 2090 | 10092 | 2222 | 2398 | 3065 | 3958 | 2381 | 2273 | 2163 | 2058 | 1896 |
| clang-14 -O2 | 2075 | 10127 | 2090 | 10094 | 2222 | 2399 | 3068 | 3959 | 2381 | 2273 | 2163 | 2058 | 1896 |
| clang-14 -Os | 2073 | 10126 | 2113 | 10103 | 2222 | 2399 | 3065 | 3958 | 2382 | 2273 | 2162 | 2058 | 1896 |
| clang-14 -O | 2074 | 10125 | 2123 | 10151 | 2222 | 2399 | 3068 | 3958 | 2381 | 2273 | 2162 | 2058 | 1896 |
| clang-17 -O3 | 2075 | 10128 | 2106 | 10078 | 2220 | 2399 | 3057 | 3958 | 2382 | 2274 | 2163 | 2058 | 1896 |
| clang-17 -O2 | 2074 | 10126 | 2105 | 10075 | 2220 | 2399 | 3056 | 3958 | 2381 | 2273 | 2162 | 2058 | 1896 |
| clang-17 -Os | 2074 | 10124 | 2093 | 10068 | 2220 | 2399 | 3071 | 3958 | 2381 | 2273 | 2162 | 2057 | 1896 |
| clang-17 -O | 2074 | 10128 | 2142 | 10142 | 2220 | 2400 | 3062 | 3958 | 2382 | 2274 | 2162 | 2058 | 1896 |
| clang-18 -O3 | 2074 | 10124 | 2098 | 10085 | 2226 | 2399 | 3060 | 3958 | 2381 | 2273 | 2172 | 2058 | 1896 |
| clang-18 -O2 | 2074 | 10127 | 2098 | 10084 | 2219 | 2399 | 3061 | 3958 | 2381 | 2273 | 2162 | 2058 | 1896 |
| clang-18 -Os | 2073 | 10126 | 2098 | 10085 | 2220 | 2399 | 3056 | 3958 | 2381 | 2273 | 2162 | 2058 | 1896 |
| clang-18 -O | 2075 | 10128 | 2149 | 10154 | 2220 | 2399 | 3061 | 3959 | 2382 | 2274 | 2162 | 2058 | 1896 |

## best per contender (what SUPERCOP's autotuner would pick)

Best = lowest **median** across the sweep. `this/microcode` = this_contender_median /
microcode_median; >1 means microcode is faster (e.g. 1.07x = ~7% faster; 1.000x is
microcode itself). p10/p90 are taken at each contender's best-median config.

| contender | median | min | p10 | p90 | winning config | this/microcode |
|---|---|---|---|---|---|---|
| keccak/x86_64_asm | 2072 | 2064 | 2071 | 2077 | gcc-11 -Os | 1.093x |
| keccak/x86_64_shld | 10109 | 10101 | 10104 | 10147 | gcc-12 -O3 | 5.332x |
| keccak/opt64lcu24 | 2055 | 2045 | 2053 | 2062 | gcc-11 -Os | 1.084x |
| keccak/opt64lcu24shld | 10068 | 10063 | 10064 | 10093 | clang-17 -Os | 5.310x |
| keccak/opt64lcu6 | 2219 | 2218 | 2219 | 2226 | clang-18 -O2 | 1.170x |
| keccak/opt64u6 | 2398 | 2397 | 2398 | 2408 | gcc-11 -O3 | 1.265x |
| keccak/sseu2 | 3056 | 3050 | 3051 | 3061 | clang-17 -O2 | 1.612x |
| keccak/mmxu1 | 3958 | 3938 | 3953 | 3965 | clang-14 -O3 | 2.088x |
| keccak/simple | 2373 | 2372 | 2373 | 2380 | gcc-13 -O2 | 1.252x |
| keccak/xkcp_g64 | 2273 | 2271 | 2272 | 2280 | clang-14 -O3 | 1.199x |
| keccak/xkcp_g64lc | 2160 | 2152 | 2159 | 2163 | gcc-12 -O3 | 1.139x |
| keccak/openssl | 2055 | 2048 | 2051 | 2060 | gcc-12 -Os | 1.084x |
| keccak/microcode | 1896 | 1889 | 1890 | 1899 | gcc-11 -O3 | 1.000x |

## headline

- fastest SUPERCOP baseline: **keccak/opt64lcu24 = 2055 cyc/perm** (median, gcc-11 -Os)
- microcode (looped): **1896 cyc/perm** (median, gcc-11 -O3)
- ratio microcode / fastest SUPERCOP: **0.923x** (microcode WINS)
