# Paper_probes

The hardware probes that establish the constraints stated in the methodology
section of the paper, together with the investigations those constraints were
distilled from. Gathered here in two sweeps — the constraint set on 2026-09-10,
the rest on 2026-09-12 — from `simple/`, `tests/`, `simple/keccak/probes/` and
`simple/curve25519/`, so that the evidence behind each claim sits in one place
and can be re-run as a set.

Almost every probe installs a microcode patch and therefore needs root, a
red-unlocked CPU, and a pinned core. The exceptions are the few pure-software
checks noted as such in the tables below. Nothing here is safe to quote from an unpinned run: the
timestamp counter advances at the 1.10 GHz nominal rate while the core bursts to
about 2.3 GHz, so an unpinned measurement is scaled by an unknown factor.

```
sudo bash ../pin_cpu.sh          # turbo off, userspace governor at base. Resets on reboot.
make all                         # the 76 probes that live here
make constraints                 # just the 26 the methodology section cites
make fieldop / carry / stgbuf / mech   # one investigation at a time
make delegated                   # the 6 that must build in ../curve25519
sudo taskset -c 0 ./probe_memops_static 1
```

## Constraint to probe map

Claim wording follows the methodology itemize. "Recorded" is the result already
in the project notes, not a result re-observed today.

### C1 Internal state

Sixteen temporary registers beyond the architectural file, condition state
attached to individual destination registers, both lost on return.

| Probe | Establishes | Recorded |
|---|---|---|
| `probe_rsp.c` | `RSP` is usable as a further data register when saved and restored once per firing | PASS |
| `carrystate_scope.c` | lifetime and scope of the per-register condition state | see notes |
| `arch_cf_survival.c` | whether a bridged architectural carry survives across triads | see notes |
| `probe_carry.c` | `GENARITHFLAGS_RR` bridges a computed carry into a following `ADC`, with a matched control | arms 6 and 7 |
| `mul_dst_class.c` | which register classes a multiply may write | dst is free |
| `tmp_persistence.c` | whether a temporary's value and its condition state survive a return to the architectural interface, and what destroys them | **claim refuted**, see below |

### C2 Micro-operation vocabulary

| Probe | Establishes | Recorded |
|---|---|---|
| `probe_opsem.c` | `IMUL64L` (0x264) is a non-destructive low multiply; `MUL_DSZ64_DRR` preserves srcA, writes lo into srcB and hi into any register; size field is bits 7:6 | verified |
| `probe_rol.c` | `ROL_DSZ64_DRI` rotates by a constant in one operation | works |
| `probe_shrd.c` | there is no usable 64 bit funnel shift; 0x997 is a 32 bit shift and 0x957 halted the machine | negative |

### C3 Fixed cost of crossing into patch RAM

| Probe | Establishes | Recorded |
|---|---|---|
| `probe_vmwrite_cost.c` | bare redirection with no operand traffic | about 7 cycles |
| `probe_hookcost.c` | the same cost measured from the Keccak harness | consistent |
| `probe_trigger.c` (delegated) | the choice of hooked instruction and match entry index | worth 0.0 cycles |
| `probe_trigger_class.c` | the same question across instruction families, not just the VMX pair | 0.00 spread, see below |
| `probe_wrapper.c` (delegated) | the operand marshalling share of a complete invocation | about 16 cycles total |

#### The crossing is instruction-agnostic, 2026-09-11

`probe_trigger.c` had shown vmwrite and vmread agreeing to 0.0 cycles, but both
are VMX instructions reaching the microsequencer by the same route, so the
result only established that two members of one family agree. `probe_trigger_class.c`
repeats the measurement across entry points: one triad (`ZEROEXT rdi, rdi`) at
U7c00, a dependent chain through `RDI`, no memory traffic, only the hooked
instruction varying.

| trigger | MSROM entry | cyc/call | minus the triad |
|---|---|---:|---:|
| `vmwrite` | `0x0cd8` | 7.97 | **6.97** |
| `vmread` | `0x0618` | 7.97 | **6.97** |
| `rdseed` | `0x0430` | 7.97 | **6.97** |

Spread 0.00 to two decimals. `rdseed` is the arm that matters: a different
opcode map entry, a single destination operand instead of two sources, and an
MSROM entry point far from the VMX pair. It costs exactly the same. The match
compares an MSROM address, and by the time it fires the instruction identity
has already been resolved into that address, so nothing about which instruction
produced it reaches the cost.

Two consequences. First, this closes the "cheaper hook instruction" idea listed
as untested at `loss-againstamd64-64.md:203` — it is now measured, and the
answer is that there is nothing to win. Second, it corroborates the seven-cycle
figure itself from three independent hooks, against `probe_vmwrite_cost`'s
single one.

Note on what the number is: the probe patch reads and writes `RDI`, so
consecutive firings carry a true RAW dependency. 6.97 is therefore
firing-to-firing **latency with the operands already in registers**, not a
throughput figure. The gap to the 16.18 invocation floor is what appears when
operands are routed through memory instead.

Arm 4 (`rdrand`, `0x0428`) is deliberately not part of the default run: glibc
and the kernel execute it, so the hook redirects their calls too. It is also
now redundant — `rdrand` is `0F C7 /6` against `rdseed`'s `0F C7 /7`, the same
opcode with a different modrm reg field, so it tests nothing `rdseed` has not.
`rdtscp` (`0x0788`) and `pause` (`0x0bf0`) must never be hooked: the vDSO uses
the first for `clock_gettime` and every spin loop in the system uses the second.

### C4 Bounded patch capacity

| Probe | Establishes | Recorded |
|---|---|---|
| `probe_keccak_capacity.c` | usable triads for a single resident kernel | 128 |
| `patch_ram_capacity.c` | extent of patch RAM and the four address units per triad | U7c00 to U7dfc |

### C5 Static schedule and sequential slot semantics

| Probe | Establishes | Recorded |
|---|---|---|
| `test_raw_war_waw.c` | RAW, WAR and WAW across all three ordered slot pairs for an architectural and a temporary register, 18 sub-tests, all with `ZEROEXT_DSZ64_DR` | all pass, later slot wins on WAW |
| `test_slot02_raw.c` | slot 0 to slot 2 value forwarding with `ADD`, `MUL` and `SHR` | works |
| `test_slot_raw_extended.c` | `MUL` srcB low half visible to slot 1; `SETCC` then `ADD` then `ADD` reading slot 1 | works |
| `test_mul_slot2.c` | `MUL` is legal in slots 0, 1 and 2 | works |
| `probe_triad.c` | seven dependency patterns with `XOR` and `ROL`, including a 3 deep chain inside one triad | pass rows are safe to pack |

### Recheck of the C1 loss claim

The sentence "temporary registers and their associated state cease to be
available once execution returns to the architectural interface" is asserted in
`CLAUDE.md`, `SUMMARY.md`, `microcode_findings.md` section 3,
`sq_pair_design.md`, `methodology.tex` ("no temporary register survives a
firing") and `keccak/docs/section3_microcode_layer.tex`. None of them cites a
probe. The only code that ever touched it is `test_two_patch.c` "Test 4", which
writes one temporary, reads it back through a second hook, and has no recorded
verdict. `SUMMARY.md` states the rule two lines below "MUL must be in slot 0"
and "SETCC only works on TMP registers", both of which this project has since
refuted (`test_mul_slot2.c`, `carrystate_scope.c` group B), so the provenance is
the same folklore that produced two known-wrong rules.

The claim also bundles three separable propositions, and the wording asserts the
third:

* the **value** in a temporary is gone at the next firing;
* the **condition state** attached to it is gone;
* the loss is caused by **the return itself** rather than by whatever code runs
  between two firings.

If temporaries survive a back-to-back firing pair and are destroyed only by
intervening microcoded work, the first two can still hold in practice while the
third is false, and the paper is then wrong about the mechanism rather than
about the consequence. `tmp_persistence.c` separates them: section 1 fires one
patch three times with no instruction in between, section 2 sweeps nine fillers
from nothing up to a context switch across a two-hook A/B pair, and section 3
asks the same of a latched carry against a single-firing control. Every
temporary is loaded with a per-run random seed plus its own index, so a survival
verdict cannot coincide with a stale ROM constant and it names the register.

#### Result, 2026-09-11: the claim is false as stated

Run on the pinned core, seed `0xc534f48eb54abb00`.

**Section 1. Every temporary survives.** Three consecutive firings of one patch
with no instruction between them: 16/16 temporaries still held `seed+i` at the
next firing, and again at the one after. Block 0 (the register file the install
helpers left behind) matches nothing, which confirms the harness can tell a
survivor from a stale value.

**Section 3. The condition state survives too.** A latches a carry on `TMP0` and
returns; B asks for it with `SETCC` in a later firing and gets 1 for the
carrying operand pair and 0 for the non-carrying one, matching the
single-firing control exactly. The value witness in `TMP2` also came back
intact. Both halves of the C1 sentence are wrong.

**Section 2. What actually destroys them is other microcode.** Survival across
an A/B pair, by filler:

| Filler | Survived | Mask, `TMP0` on the left |
|---|---|---|
| nothing at all | 16/16 | `################` |
| 8 x nop | 16/16 | `################` |
| 5 ALU ops | 16/16 | `################` |
| 3 memory ops | 16/16 | `################` |
| a call to an empty function | 16/16 | `################` |
| `div r32` | 14/16 | `#####..#########` |
| `lock xadd` | 13/16 | `...#############` |
| `rdtsc` | 12/16 | `.#..#########.##` |
| `pushfq` / `popfq` | 10/16 | `......##########` |
| `cpuid` | 7/16 | `...###......####` |
| `getpid()` | 7/16 | `.......##.###.##` |
| `sched_yield()` | 7/16 | `.......##.###.##` |
| `nanosleep` 1 ms | 0/16 | `................` |
| `printf` | 0/16 | `................` |

Plain architectural instructions, including the loads and stores and address
arithmetic that a wrapper puts between two firings, leave the whole temporary
file untouched. Every loss is caused by another *microcoded* flow reusing the
same registers, and each such flow has its own footprint: `pushfq`/`popfq`
takes `TMP0` to `TMP5` and nothing above, `div` takes only `TMP5` and `TMP6`,
`cpuid` takes nine of them. `getpid` and `sched_yield` produce the *identical*
mask, which is the kernel entry and exit path rather than the call, so the
footprints look deterministic rather than random. Low-numbered temporaries are
clobbered by the most flows, but no register is safe from all of them:
`rdtsc` reaches `TMP13`.

**What this means for the paper.** The consequence the rule protects is intact,
but its stated cause is not. Temporaries are not architected, so nothing saves
or restores them across a return; the operating system cannot preserve state it
cannot name. Any interrupt, syscall or context switch therefore runs microcode
that overwrites them, which is why `nanosleep` and `printf` reach 0/16. A patch
still may not *rely* on a temporary across a firing, but the reason is that the
window is asynchronously interruptible, not that the return clears anything.
The 16/16 rows are "no interrupt arrived during this window", not a guarantee.

**Open, and worth one more probe.** How often a back-to-back pair actually
loses its temporaries is unmeasured, and it is the number that decides whether
a sentinel-and-recompute scheme could ever carry live state across firings.
A follow-up should repeat the section 2 masks many times to confirm the
footprints are deterministic, spin in a pure-ALU loop long enough to guarantee
a timer tick and show the loss, and count losses over enough back-to-back
trials to put a rate on the hazard.

### C6 Slots share execution resources — RECHECK TODAY

| Probe | Establishes | Recorded |
|---|---|---|
| `probe_memops.c` | one isolated triad holding two memory operations, four arms (2 loads, 2 stores, load+store, store then load same address) | all four PASS, see `probe_memops.log` |
| `probe_stlf.c` | store to load forwarding inside a patch | works |
| `probe_loaduse.c` | a load result consumed by a later slot of the same triad | no recorded verdict |
| `probe_sched.c` (delegated) | the `mulpack` family: multiplies spread versus clustered | 1.328 against 1.964 cycles, +48% |
| `probe_issue.c` (delegated) | superseded issue-rate measurement, kept because its 0.153 figure is quoted in older notes and is wrong | refuted |

### Recheck of 2026-09-10

Both C6 rules were re-run on hardware. The harness validity check passes first:
the triad sweep's intercept is 16.15 cycles against the 16.18 cycle dispatch
floor measured independently, and the pure ALU arms land on 0.996 cyc/triad. An
unpinned run would have reported roughly 0.48 of these figures, so the agreement
also confirms the core was pinned and that the red unlock survived the reboot.

**Multiply rule CONFIRMED, reproduces to three decimals.** `probe_sched`
decomposes as follows. A spread triad holds one multiply and two ALU operations
and costs 1.992, so a multiply costs 1.992 minus 2 x 0.332 = **1.328**. The
clustered arm puts its 42 multiplies into 14 triads of three and leaves 28 pure
ALU triads, so 42 x 2.628 minus 28 x 0.996 = 82.5 over 14 triads = 5.892 per
three multiply triad = **1.964** per multiply. The penalty is **+47.9%**, and
both figures match the 2026-09-09 record exactly.

Two further results reproduce. The `regs` sweep is flat from R=1 to R=16, so the
register file is fully renamed, WAW and WAR false dependencies are free, and
scratch rotation in the generator is decoration. The `width` sweep saturates by
W=5 at 0.996 cyc/triad, so the five accumulator production schedule already
reaches the issue limit and more parallelism would buy nothing.

Caveat on the `dist` family. It measures 2.93 to 2.98 cyc/triad and is flat in D,
which is within a few percent of the fully serial bound of 0.997 cyc/op. Flat in
D is also the signature of the RAW chain defect its own header describes as
fixed. Treat this family as uninterpreted until that is checked.

**Memory rule, isolated case CONFIRMED.** All four `probe_memops` arms PASS
again: two loads, two stores, load with store, and store then load to the same
address. The last arm also demonstrates store to load forwarding inside a single
triad.

**The gap. `probe_memops` shows that a *single* two memory triad works. The
production rule of one memory operation per triad comes from a different
observation: mass-packing many two memory triads in the Keccak prologue hard
crashed the machine on 2026-06-29. No probe currently brackets the threshold
between those two facts, so the paper's wording rests on a policy rather than on
a measured limit. Closing that gap needs a graded probe that fires N
back-to-back two memory triads for increasing N. **That experiment is the one
that previously took the machine down.** Do not run it without deciding that the
risk is acceptable, and log every attempt with an fsync before firing, the way
`probe_memops.c` and `probe_shrd.c` already do.

### C7 The programming model is undocumented

| Probe | Establishes | Recorded |
|---|---|---|
| `probe_offset.c` | the displacement of `LDZX` and `STAD` is an 8 bit signed field, so the base register is centred | -128 to +127 |
| `probe_seg.c` | `SEG_DS` (0x18) is the working segment; a wrong segment crashed the box | 0x18 |
| `probe_loop.c` | backward `UJMPCC` works, count up, comparison in the same triad | works |
| `ujmp_test.c` | `UJMPCC` evaluates the zero flag rather than its register operand; `SUB` immediate is reversed | works |
| `probe_oplat.c` (delegated) | dependent chain latency per operation | ADD 1.0, ZEROEXT 0.7, SETCC 1.9, MUL 5.9 |
| `probe_ldorder.c` (delegated) | the caller's load order against the previous store | descending order costs about 30 cycles |

## The investigations behind the constraints

Second sweep, 2026-09-12. The constraint set above is a distillation; these are
the experiments it was distilled from, moved here from `simple/` and `tests/` so
that no hardware experiment is left outside this directory. They are working
notes in code. Several are dead ends, kept because a dead end is the answer to
"did you try", and several were superseded — where one of these disagrees with
the constraint map above, the constraint map is the later word and wins.

Build a group on its own with `make fieldop`, `make carry`, `make stgbuf` or
`make mech`.

### Where the field-op cost goes — `make fieldop`

From `simple/`. These ask what a firing is actually paying for, which is the
question behind the firing-tax model and the fewer-firings ladder redesign.

| Probe | Asks | Recorded |
|---|---|---|
| `probe_sq_latency.c` | does firing latency scale with triad count, or is it overhead-bound | about 80 cycles of fixed tax plus the critical path |
| `probe_interleave.c` | whether filler triads hide in stalls: the same pad appended against interleaved | companion to the above, same harness |
| `probe_looped_fieldop.c` | a field op looped inside one firing, so the tax is paid once | looped `fe_sq` 82 cyc against native 86; `fe_mul` 118 |
| `probe_mul_critpath.c` | is the looped `fe_mul`'s 1.64 cyc/triad a serial critical path or the sequencer's per-triad throughput | reschedulable or not, see the header |
| `probe_sqmul.c` | the real firing latency of a fused two-output `sqmul(X,Y)` | about 1.15x a single op |
| `probe_loopcost.c` | the cost of loop control alone, empty counted loop swept in N | slope is the branch and counter cost |
| `probe_mul_isolate.c` | is the field-op edge the `MUL` primitive | no |
| `probe_carry_isolate.c` | is it instead the carry-chain handling | companion to the above |
| `probe_mul_carry_style.c` | or is it triad packing density, measured in the latency regime the ladder pays | the conclusive arm of the three |

### Carry, flags and the bridge — `make carry`

From `tests/`, with `test_adc.c` from `simple/`. This is the whole ADD / SETCC /
ADC / `GENARITHFLAGS` investigation. `additionwithflags.c` is the consolidated
master test and the one the paper cites — 45/45 certifying PASS on 2026-09-07;
read it first and treat the rest as its provenance.

| Probe | Asks |
|---|---|
| `additionwithflags.c` | master test: the whole flag model in eight sections with expected values |
| `subtractionwithflags.c` | the same for the borrow path; first characterisation of `SBB_DSZ64` (0x37f) |
| `flags.c` | intra-triad `SETCC_CONDB` after `ADD` |
| `test_setcc_repeated.c` | whether `SETCC_CONDB` still works across repeated firings |
| `simple_ADC.c`, `_r64.c`, `_tmp.c` | the smallest possible ADC tests: plain, R64SRC/R64DST encodings, TMP sources |
| `intra-triad-adc.c` | does a slot-0 `ADD` give a slot-1 `ADC` its CF inside one triad |
| `intra-triad-adc-regclass.c` | does that depend on the ADD's destination register class |
| `intra-triad-adc-genflagsrr.c` | `GENARITHFLAGS_RR(src0, src1)` as the actual bridge |
| `test_adc_gfl_adc_intratriad.c` | packing `{ADC, GFL_RR, ADC}` in one triad and keeping the bridge |
| `genflagsrr_exhaustive.c` | every placement of `GENARITHFLAGS` relative to `ADD` and `ADC` |
| `gfl_rr_operand_matrix.c` | which operand forms bridge TMP-CF to arch CF |
| `genflagsrr_bridge_verify.c` | confirms `GENARITHFLAGS_RR(TMP, TMP)` is a real bridge |
| `genflagsrr_final.c` | the definitive run of the above |
| `arch_gfl_chain.c` | whether `GENARITHFLAGS_RR(arch, arch)` bridges as reliably — it does not |
| `chain_length.c` | does the bridged chain scale to arbitrary length |
| `chained_4limb_add.c` | a 256-bit add built from the chain |
| `test_genarithflags_semantics.c` | does `GENARITHFLAGS` read TMP-CF or the TMP value |
| `test_adc_dsz64.c` | validates the speculative `ADC_DSZ64` = 0x37e opcode |
| `test_adc_chain.c` | ADC chaining for 4x64 viability |
| `test_adc_carry_route.c` | can ADD's CF be routed into ADC at all |
| `test_adc.c` | narrows a `GENARITHFLAGS` carry leak from the previous triad |
| `readaflags_probe.c` | what `READAFLAGS` actually reads |
| `does_add_update_eflags.c` | does a microcode `ADD` write any arch flag register `READAFLAGS` sees |
| `test_movetocreg_eflags.c` | can `MOVETOCREG` write arch EFLAGS directly — dead end |
| `cr_cf_probe.c` | can any `MOVETOCREG` variant flip arch CF mid-patch — dead end |
| `adcx_probe.c` | sweeps unknown opcodes for ADCX/ADOX-like semantics, one opcode per process, run under `timeout` because a hit can hang the core. Driver `sweep_adcx.sh`, output `adcx_sweep.log`, `adcx_hits.log`, `adcx_hangs.log` |

### The staging buffer — `make stgbuf`

Whether `STADSTGBUF` / `LDSTGBUF` can serve as an extended register file. The
recorded answer is no: stores are free but a load is about 30 cycles and does
not pipeline.

| Probe | Asks |
|---|---|
| `stgbuf_probe.c` | minimum reliable stride and the number of distinct usable slots |
| `stgbuf_test.c` | the buffer as an extended TMP or spill area |
| `stgbuf_bench.c` | cycles per `LDSTGBUF` / `STADSTGBUF`, by incremental cost |
| `stgbuf_overlap.c` | does the load latency overlap with non-stgbuf compute or serialize |

### Patch RAM, the hook and the vocabulary — `make mech`

| Probe | Asks |
|---|---|
| `probe_patchram.c` | usable patch RAM size |
| `multi_capacity_test.c` | usable triad capacity per free region |
| `jump_test.c` | cross-region patch RAM jumping |
| `dump_slots.c` | dumps all 32 match-and-patch slots with seqword bank analysis |
| `test_two_patch.c` | the two-patch vmwrite/vmread mechanism, minimal case |
| `test_intradata.c` | can slot 1 read what slot 0 writes in the same triad (superseded by `test_raw_war_waw.c`) |
| `test_ldzx.c` | `LDZX` smoke test |
| `test_2mac_prereqs.c` | prerequisites for a 2-MAC-per-firing hook |
| `test_x25519_debug.c` | which X25519 primitive is broken, with no microcode installed — needs no root |

## Delegated probes

`probe_sched`, `probe_issue`, `probe_oplat`, `probe_ldorder`, `probe_trigger`
and `probe_wrapper` include `full_curve25519_inline2.c` and link roughly thirty
contender objects plus SUPERCOP's `libcryptoint.a`. Their sources stay in
`../curve25519`; `make delegated` builds them there so this directory is still
the single entry point. Run them from `../curve25519`.

## Provenance

Sources moved with `git mv` where tracked, and compiled binaries carried along
with them. Include paths were rewritten from their original depths to
`../../../include/`, and the Makefile also passes `-I../../../include` so probes
written with a bare `#include "patch.h"` build unchanged. All 76 sources were
compile-checked from this directory after the move.

Recorded output kept alongside: `probe_memops.log`, `probe_carry_out.txt`,
`probe_opsem_out.txt`, `probe_shrd_out.txt`, `probe_trigger_class.log`,
`tmp_persistence.log`, `probe_seg.log`, and the three `adcx_*.log` sweeps.

What deliberately stayed behind. `tests/` keeps the implementation benchmarks
(`bench_2mac.c`, `bench_mac3t*.c`, `bench_mul.c`, `bench_sq_compare.c`,
`bench_hybrid.c`, `bench_curve25519.c`, `ultimate_bench.c`) — they measure field
arithmetic implementations rather than the hardware, and they need the CryptOpt
`.asm`/`.o` objects that sit next to them. It also keeps `adc_findings.md` and
`patch_ram_findings.md`, which `CLAUDE.md` cites at their present paths.
`simple/keccak/probes/` keeps `probe_index.c` and `probe_keccak_io.c`.
