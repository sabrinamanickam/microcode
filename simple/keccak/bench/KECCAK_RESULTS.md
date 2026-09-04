# Keccak SUPERCOP-matrix benchmark

Same methodology as `../bench_supercop_matrix.sh`: for each (compiler, -O)
config SUPERCOP tries, rebuild every runnable SUPERCOP Keccak baseline (hand
asm, 64-bit C, x86 SIMD, reference) + the
head-to-head harness, run back-to-back in one process, keep the best per
contender. cyc/perm (RDTSC at TSC rate; pin to base so ticks ≈ true cycles).
Headline statistic is the **median**; min and the p10–p90 spread are also reported.

Configs that ran: 24 / 24

Environment: base-pinned, turbo off. Delivered core freq 1100 MHz,
TSC (RDTSC) rate 1094 MHz, correction f_core/f_TSC = 1.00548
(aperf/mperf under load, verified before the sweep; ratios invariant to it, multiply
absolute cycle counts by it for true core cycles).

## median cyc/perm per (config × contender)

| config | asm | shld | opt24 | opt24shld | lcu6 | u6 | sse | mmx | simple | xg64 | xg64lc | ossl | ucode |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 2076 | 10121 | 2162 | 10170 | 2223 | 2398 | 3095 | 3960 | 2372 | 2275 | 2161 | 2054 | 1895 |
| gcc-11 -O2 | 2076 | 10122 | 2163 | 10171 | 2223 | 2397 | 3098 | 3960 | 2374 | 2275 | 2161 | 2054 | 1895 |
| gcc-11 -Os | 2073 | 10125 | 2055 | 10077 | 2224 | 2397 | 3094 | 3959 | 2373 | 2275 | 2161 | 2055 | 1895 |
| gcc-11 -O | 2076 | 10127 | 2132 | 10195 | 2223 | 2398 | 3101 | 3960 | 2374 | 2276 | 2162 | 2055 | 1896 |
| gcc-12 -O3 | 2077 | 10127 | 2261 | 10286 | 2221 | 2398 | 3095 | 3960 | 2375 | 2276 | 2161 | 2055 | 1895 |
| gcc-12 -O2 | 2077 | 10123 | 2260 | 10284 | 2221 | 2399 | 3095 | 3961 | 2373 | 2276 | 2161 | 2054 | 1895 |
| gcc-12 -Os | 2076 | 10119 | 2161 | 10188 | 2221 | 2398 | 3101 | 3960 | 2373 | 2275 | 2160 | 2054 | 1895 |
| gcc-12 -O | 2077 | 10122 | 2241 | 10320 | 2221 | 2399 | 3112 | 3961 | 2374 | 2275 | 2161 | 2054 | 1895 |
| gcc-13 -O3 | 2076 | 10120 | 2235 | 10259 | 2221 | 2399 | 3095 | 3961 | 2373 | 2276 | 2161 | 2054 | 1895 |
| gcc-13 -O2 | 2077 | 10123 | 2234 | 10261 | 2221 | 2398 | 3099 | 3961 | 2373 | 2275 | 2161 | 2054 | 1895 |
| gcc-13 -Os | 2075 | 10119 | 2144 | 10185 | 2221 | 2398 | 3101 | 3960 | 2374 | 2275 | 2160 | 2054 | 1895 |
| gcc-13 -O | 2077 | 10120 | 2224 | 10315 | 2221 | 2398 | 3095 | 3961 | 2373 | 2275 | 2161 | 2054 | 1895 |
| clang-14 -O3 | 2075 | 10120 | 2090 | 10090 | 2221 | 2399 | 3068 | 3955 | 2380 | 2275 | 2162 | 2057 | 1895 |
| clang-14 -O2 | 2075 | 10121 | 2091 | 10093 | 2221 | 2398 | 3064 | 3956 | 2381 | 2274 | 2162 | 2057 | 1895 |
| clang-14 -Os | 2075 | 10121 | 2115 | 10102 | 2221 | 2398 | 3067 | 3956 | 2380 | 2274 | 2162 | 2057 | 1896 |
| clang-14 -O | 2075 | 10121 | 2124 | 10152 | 2221 | 2398 | 3064 | 3955 | 2380 | 2274 | 2163 | 2057 | 1895 |
| clang-17 -O3 | 2075 | 10118 | 2106 | 10079 | 2219 | 2398 | 3058 | 3956 | 2380 | 2274 | 2163 | 2057 | 1896 |
| clang-17 -O2 | 2075 | 10116 | 2106 | 10077 | 2219 | 2399 | 3055 | 3956 | 2380 | 2274 | 2163 | 2056 | 1895 |
| clang-17 -Os | 2075 | 10117 | 2094 | 10069 | 2219 | 2398 | 3055 | 3956 | 2380 | 2274 | 2163 | 2057 | 1895 |
| clang-17 -O | 2075 | 10118 | 2142 | 10138 | 2219 | 2398 | 3055 | 3956 | 2380 | 2274 | 2163 | 2057 | 1895 |
| clang-18 -O3 | 2075 | 10117 | 2099 | 10086 | 2219 | 2400 | 3058 | 3956 | 2381 | 2274 | 2163 | 2056 | 1895 |
| clang-18 -O2 | 2075 | 10115 | 2099 | 10084 | 2219 | 2398 | 3055 | 3956 | 2380 | 2274 | 2163 | 2056 | 1895 |
| clang-18 -Os | 2074 | 10116 | 2098 | 10082 | 2219 | 2398 | 3059 | 3955 | 2380 | 2274 | 2163 | 2056 | 1895 |
| clang-18 -O | 2075 | 10115 | 2149 | 10149 | 2219 | 2398 | 3059 | 3956 | 2380 | 2274 | 2163 | 2057 | 1895 |

## best per contender (what SUPERCOP's autotuner would pick)

Best = lowest **median** across the sweep. `this/microcode` = this_contender_median /
microcode_median; >1 means microcode is faster (e.g. 1.07x = ~7% faster; 1.000x is
microcode itself). p10/p90 are taken at each contender's best-median config.

| contender | median | min | p10 | p90 | winning config | this/microcode |
|---|---|---|---|---|---|---|
| keccak/x86_64_asm | 2073 | 2064 | 2072 | 2084 | gcc-11 -Os | 1.094x |
| keccak/x86_64_shld | 10115 | 10105 | 10107 | 10152 | clang-18 -O2 | 5.338x |
| keccak/opt64lcu24 | 2055 | 2045 | 2051 | 2073 | gcc-11 -Os | 1.084x |
| keccak/opt64lcu24shld | 10069 | 10059 | 10061 | 10099 | clang-17 -Os | 5.313x |
| keccak/opt64lcu6 | 2219 | 2217 | 2218 | 2227 | clang-17 -O3 | 1.171x |
| keccak/opt64u6 | 2397 | 2396 | 2397 | 2431 | gcc-11 -O2 | 1.265x |
| keccak/sseu2 | 3055 | 3048 | 3049 | 3064 | clang-17 -O2 | 1.612x |
| keccak/mmxu1 | 3955 | 3943 | 3950 | 3968 | clang-14 -O3 | 2.087x |
| keccak/simple | 2372 | 2371 | 2372 | 2383 | gcc-11 -O3 | 1.252x |
| keccak/xkcp_g64 | 2274 | 2271 | 2273 | 2304 | clang-14 -O2 | 1.200x |
| keccak/xkcp_g64lc | 2160 | 2152 | 2160 | 2198 | gcc-12 -Os | 1.140x |
| keccak/openssl | 2054 | 2047 | 2054 | 2065 | gcc-11 -O3 | 1.084x |
| keccak/microcode | 1895 | 1889 | 1890 | 1929 | gcc-11 -O3 | 1.000x |

## headline

- fastest SUPERCOP baseline: **keccak/openssl = 2054 cyc/perm** (median, gcc-11 -O3)
- microcode (looped): **1895 cyc/perm** (median, gcc-11 -O3)
- ratio microcode / fastest SUPERCOP: **0.923x** (microcode WINS)
