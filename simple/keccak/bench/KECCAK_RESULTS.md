# Keccak SUPERCOP-matrix benchmark

Same methodology as `../bench_supercop_matrix.sh`: for each (compiler, -O)
config SUPERCOP tries, rebuild every runnable SUPERCOP Keccak baseline (hand
asm, 64-bit C, x86 SIMD, reference) + the
head-to-head harness, run back-to-back in one process, keep the best per
contender. cyc/perm (RDTSC at TSC rate; pin to base so ticks ≈ true cycles).

Configs that ran: 24 / 24

## min cyc/perm per (config × contender)

| config | asm | shld | opt24 | opt24shld | lcu6 | u6 | sse | mmx | simple | ucode |
|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 2063 | 10102 | 2153 | 10151 | 2206 | 2401 | 3137 | 3878 | 2495 | 1912 |
| gcc-11 -O2 | 2062 | 10102 | 2154 | 10153 | 2206 | 2400 | 3134 | 3878 | 2496 | 1911 |
| gcc-11 -Os | 2062 | 10098 | 2047 | 10059 | 2209 | 2402 | 3134 | 3868 | 2511 | 1911 |
| gcc-11 -O | 2063 | 10094 | 2125 | 10170 | 2206 | 2401 | 3136 | 3872 | 2506 | 1913 |
| gcc-12 -O3 | 2064 | 10102 | 2257 | 10264 | 2206 | 2401 | 3134 | 3874 | 2495 | 1912 |
| gcc-12 -O2 | 2064 | 10102 | 2258 | 10264 | 2206 | 2401 | 3134 | 3874 | 2495 | 1912 |
| gcc-12 -Os | 2066 | 10097 | 2158 | 10171 | 2208 | 2403 | 3070 | 3878 | 2513 | 1910 |
| gcc-12 -O | 2065 | 10107 | 2239 | 10296 | 2206 | 2401 | 3132 | 3867 | 2507 | 1912 |
| gcc-13 -O3 | 2064 | 10104 | 2233 | 10242 | 2206 | 2401 | 3134 | 3874 | 2495 | 1912 |
| gcc-13 -O2 | 2064 | 10101 | 2232 | 10240 | 2206 | 2401 | 3137 | 3874 | 2495 | 1912 |
| gcc-13 -Os | 2066 | 10093 | 2139 | 10168 | 2207 | 2402 | 3069 | 3878 | 2512 | 1910 |
| gcc-13 -O | 2065 | 10093 | 2223 | 10295 | 2206 | 2401 | 3133 | 3867 | 2507 | 1912 |
| clang-14 -O3 | 2062 | 10102 | 2080 | 10073 | 2206 | 2401 | 3070 | 3878 | 2509 | 1911 |
| clang-14 -O2 | 2062 | 10102 | 2081 | 10073 | 2206 | 2400 | 3056 | 3874 | 2511 | 1911 |
| clang-14 -Os | 2064 | 10102 | 2105 | 10084 | 2206 | 2400 | 3061 | 3875 | 2494 | 1915 |
| clang-14 -O | 2062 | 10102 | 2115 | 10133 | 2204 | 2400 | 3066 | 3873 | 2513 | 1912 |
| clang-17 -O3 | 2062 | 10101 | 2095 | 10061 | 2206 | 2401 | 3067 | 3874 | 2511 | 1911 |
| clang-17 -O2 | 2062 | 10102 | 2096 | 10061 | 2206 | 2401 | 3058 | 3874 | 2511 | 1911 |
| clang-17 -Os | 2062 | 10102 | 2085 | 10053 | 2206 | 2400 | 3060 | 3874 | 2493 | 1915 |
| clang-17 -O | 2062 | 10104 | 2133 | 10123 | 2206 | 2401 | 3064 | 3874 | 2512 | 1912 |
| clang-18 -O3 | 2062 | 10103 | 2088 | 10068 | 2206 | 2401 | 3066 | 3874 | 2511 | 1911 |
| clang-18 -O2 | 2062 | 10101 | 2090 | 10068 | 2206 | 2400 | 3058 | 3874 | 2511 | 1911 |
| clang-18 -Os | 2062 | 10102 | 2090 | 10069 | 2206 | 2400 | 3055 | 3875 | 2493 | 1913 |
| clang-18 -O | 2062 | 10102 | 2140 | 10132 | 2205 | 2400 | 3063 | 3874 | 2511 | 1912 |

## best per contender (what SUPERCOP's autotuner would pick)

| contender | best min | best median | winning config |
|---|---|---|---|
| keccak/x86_64_asm | 2062 | 2069 | gcc-11 -O2 |
| keccak/x86_64_shld | 10093 | 10101 | gcc-13 -Os |
| keccak/opt64lcu24 | 2047 | 2056 | gcc-11 -Os |
| keccak/opt64lcu24shld | 10053 | 10061 | clang-17 -Os |
| keccak/opt64lcu6 | 2204 | 2207 | clang-14 -O |
| keccak/opt64u6 | 2400 | 2402 | gcc-11 -O2 |
| keccak/sseu2 | 3055 | 3064 | clang-18 -Os |
| keccak/mmxu1 | 3867 | 3873 | gcc-12 -O |
| keccak/simple | 2493 | 2495 | clang-17 -Os |
| keccak/microcode | 1910 | 1915 | gcc-12 -Os |

## headline

- fastest SUPERCOP baseline: **keccak/opt64lcu24 = 2047 cyc/perm** (gcc-11 -Os)
- microcode (looped): **1910 cyc/perm** (gcc-12 -Os)
- ratio microcode / fastest SUPERCOP: **0.933x** (microcode WINS)
