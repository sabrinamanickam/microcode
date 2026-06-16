# Keccak SUPERCOP-matrix benchmark

Same methodology as `../bench_supercop_matrix.sh`: for each (compiler, -O)
config SUPERCOP tries, rebuild the SUPERCOP scalar Keccak baselines + the
head-to-head harness, run back-to-back in one process, keep the best per
contender. cyc/perm (RDTSC at TSC rate; pin to base so ticks ≈ true cycles).

Configs that ran: 24 / 24

## min cyc/perm per (config × contender)

| config | asm | shld | opt24 | opt24shld | ucode |
|---|---|---|---|---|---|
| gcc-11 -O3 | 2062 | 10089 | 2151 | 10156 | 1913 |
| gcc-11 -O2 | 2062 | 10102 | 2152 | 10154 | 1913 |
| gcc-11 -Os | 2064 | 10108 | 2045 | 10061 | 1911 |
| gcc-11 -O | 2063 | 10094 | 2122 | 10169 | 1914 |
| gcc-12 -O3 | 2063 | 10102 | 2255 | 10262 | 1913 |
| gcc-12 -O2 | 2063 | 10088 | 2255 | 10263 | 1913 |
| gcc-12 -Os | 2064 | 10106 | 2153 | 10169 | 1911 |
| gcc-12 -O | 2064 | 10106 | 2236 | 10296 | 1913 |
| gcc-13 -O3 | 2063 | 10101 | 2229 | 10240 | 1913 |
| gcc-13 -O2 | 2063 | 10086 | 2230 | 10239 | 1913 |
| gcc-13 -Os | 2065 | 10106 | 2137 | 10166 | 1910 |
| gcc-13 -O | 2064 | 10107 | 2220 | 10296 | 1914 |
| clang-14 -O3 | 2062 | 10101 | 2075 | 10071 | 1912 |
| clang-14 -O2 | 2061 | 10101 | 2077 | 10071 | 1911 |
| clang-14 -Os | 2063 | 10101 | 2101 | 10085 | 1913 |
| clang-14 -O | 2061 | 10102 | 2110 | 10133 | 1913 |
| clang-17 -O3 | 2062 | 10103 | 2092 | 10061 | 1912 |
| clang-17 -O2 | 2061 | 10103 | 2093 | 10061 | 1912 |
| clang-17 -Os | 2061 | 10103 | 2082 | 10053 | 1915 |
| clang-17 -O | 2061 | 10101 | 2129 | 10121 | 1913 |
| clang-18 -O3 | 2062 | 10104 | 2087 | 10068 | 1912 |
| clang-18 -O2 | 2062 | 10100 | 2087 | 10070 | 1912 |
| clang-18 -Os | 2061 | 10100 | 2086 | 10067 | 1913 |
| clang-18 -O | 2061 | 10100 | 2137 | 10133 | 1912 |

## best per contender (what SUPERCOP's autotuner would pick)

| contender | best min | best median | winning config |
|---|---|---|---|
| keccak/x86_64_asm | 2061 | 2069 | clang-14 -O2 |
| keccak/x86_64_shld | 10086 | 10097 | gcc-13 -O2 |
| keccak/opt64lcu24 | 2045 | 2054 | gcc-11 -Os |
| keccak/opt64lcu24shld | 10053 | 10064 | clang-17 -Os |
| keccak/microcode | 1910 | 1916 | gcc-13 -Os |

## headline

- fastest SUPERCOP baseline: **keccak/opt64lcu24 = 2045 cyc/perm** (gcc-11 -Os)
- microcode (looped): **1910 cyc/perm** (gcc-13 -Os)
- ratio microcode / fastest SUPERCOP: **0.934x** (microcode WINS)
