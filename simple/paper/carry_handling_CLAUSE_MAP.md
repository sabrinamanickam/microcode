# Carry-handling subsection: clause → probe map

Recheck of `\subsubsection{Carry Handling in Microcode}` against the probes,
2026-09-07. Rewrite: `how_carry_works.tex` (style-faithful, current). `carry_handling_REWRITE.tex` is the earlier draft and is superseded.

**The probe to cite is `Paper_probes/additionwithflags.c`** — the consolidated master
test, written specifically to be citable from the paper. Its header states the
flag model and every section asserts it with PASS/FAIL lines and known-good
expected values. Sections: (1) ADD+SETCC chain, (2) ADC reads frozen arch CF,
(3) negative controls, (4) `GENARITHFLAGS_RR` bridge, (5) ADC carry-out is
chainable, (6) end-to-end 256-bit add, (7) full flag set in domain #1,
(8) per-register carry lifetime.

**Run 2026-09-07** (Goldmont N3350, `sudo taskset -c 0`):

- `additionwithflags_static` — **45/45 certifying PASS**, "ALL PASS — flag-domain
  model verified end to end"; 27/28 exploratory probes matched (the one that
  differed is discussed under item 4 below).
- `carrystate_scope_static` — **27/27 certifying PASS**, 11/11 hypothesis probes
  matched.
- `arch_cf_survival_static` — **12/12 transparent**: once bridged, arch CF
  survives MUL, ZEROEXT, SHL, SHR, OR, ADD, SETCC and a further ADC. Only
  another `GENARITHFLAGS_RR` rewrites it.
- `mul_dst_class_static` — **57/57 PASS**: MUL's high-half destination is a free
  operand (`RCX`, a TMP, or another arch register), `srcA` preserved.
- `asm_op_curve25519_solinas_vs_static` — pinned three-arm A/B, see row 12.

Both logs should be committed alongside the paper.

| # | Clause in the text | Probe | Verdict |
|---|---|---|---|
| 1 | "`ADD` associates conditional state with its destination" | `additionwithflags.c` S1; `mac/flag_test.c` G1; **`carrystate_scope.c` Group A (new)** | **Incomplete as written; now settled.** True only for `TMP` destinations. Architectural destinations had never been probed head-on — `experiments/flag_probes.c`, cited in `EXPERIMENTS.md` as the ADD→arch failure, reads `SETCC_CONDB_DR(TMP2, TMP2)`, i.e. a register that was never an ADD destination. `test_setcc_repeated.c` does read an arch destination and fails, but it was diagnosing `END_SEQWORD` EFLAGS restore and used 51-bit limbs that cannot carry out of 64 bits. Group A now tests it directly: `ADD → R12` then `SETCC(R12)` returns **0 for every input** — carry with sum 0, carry with sum 1, and no carry — both intra-triad (A2) and cross-triad (A3), while the arithmetic result in R12 is correct in all six cases. A4 adds that an arch-destination `ADD` does not update the architectural CF either. So an architectural destination carries **no** queryable condition state, in either domain |
| 2 | "this state records whether the operation produced a carry" | `additionwithflags.c` S7; `flag_test.c` G4 (all 16 condition codes) | Confirmed, and understated: the destination latches the full arithmetic flag set (CF/ZF/SF/OF), not just carry |
| 3 | "`SETCC_CONDB` reads that state and materializes the carry as 0/1" | `additionwithflags.c` S1, S3(A) | Confirmed |
| 4 | "can then add this value to any accumulator with a subsequent `ADD`" | `additionwithflags.c` S1 (fold ADD); production patches fold into both `TMP9` and arch `R8` | Confirmed |
| 5 | "different additions can materialize carries into different registers, so several accumulation chains can progress without sharing `RFLAGS`" | was **unprobed** — nearest was `additionwithflags.c` S8 #5, exploratory, one chain plus an unrelated write; now **`carrystate_scope.c` Group C + D (new)** | **Confirmed.** C1: three ADDs into `TMP0`/`TMP2`/`TMP4`, all three carries read back in one firing, correct for **all 8** carry patterns. C2: interleaved order — chain 0's carry read *after* chain 1's ADD has run, correct for all 4 patterns, so a single shared flag register cannot be what is being read. D: a carrying `ADD`+`SETCC`+fold chain leaves the architectural CF at its entry value (RAX delta across entry CF=0/1 is exactly 1). Caveat for the prose: production exercises exactly **one** flag source — all 15 `SETCC`s in `asm_op_curve25519.c` and all 25 in `asm_op_curve25519_mul.c` are `SETCC_CONDB_DR(TMP15, TMP0)`. The mechanism permits many chains; the implementation uses one, and the text should not imply otherwise |
| 6 | "`ADC` consumes the carry flag in `RFLAGS` rather than the conditional state associated with a destination" | `additionwithflags.c` S2, S3(B)(C)(E); `test_adc_dsz64.c`; `intra-triad-adc.c`; `does_add_update_eflags.c` | Confirmed, and stronger than stated: arch CF is captured at hook entry and **frozen** — no `ADD`, and no earlier `ADC`, updates it |
| 7 | "the patch first executes `GENARITHFLAGS` on that destination" | `genflagsrr_exhaustive.c` (24 arrangements, found it); `gfl_rr_operand_matrix.c` (8-pattern operand matrix); `additionwithflags.c` S3(D), S4; `chain_length.c` (N=4,8,16,32); `arch_gfl_chain.c`; `test_genarithflags_semantics.c` | **Wrong as written.** Only `GENARITHFLAGS_RR(TMP, TMP)` bridges (equivalently `RR(TMP, any arch)`; slot 0 must be the TMP whose carry is wanted). The one-operand `GENARITHFLAGS_R` does **not** — `test_genarithflags_semantics.c` disproved the 2026-04-17 `adc_findings.md` claim, which had passed only on inputs where carry-out and a zero result coincide. Arch-register destinations never bridge (`arch_gfl_chain.c`) |
| 8 | "`GENARITHFLAGS` transfers the associated condition into `RFLAGS`, after which `ADC` adds …" | `readaflags_probe.c` P4; the ADC-side probes above | **Needs qualifying.** There are two architectural flag images. `GENARITHFLAGS_R` writes the one `READAFLAGS` observes (a faithful CF: `0x57`), which `ADC` ignores. Saying "into `RFLAGS`" without naming the operand form is exactly the ambiguity that produced the four-month-old false positive |
| 9 | "Our $5\times51$ implementation uses the first" | `asm_op_curve25519.c` (15 SETCC, 0 ADC, 0 GFL); `asm_op_curve25519_mul.c` (25 SETCC, 0 ADC, 0 GFL) | Confirmed |
| 10 | "The saturated $4\times64$ control uses the second" | `curve25519/full_curve25519_amd64_64_ucode.c` and `full_curve25519_4x64.c` (ADC + `GENARITHFLAGS_RR`, 0 SETCC); patch is v3, 75 triads, 207 cyc/op pinned; 4/4 RFC 7748 | Confirmed for the shipped control. **Note:** v4 (67 triads, 191 cyc/op) is not yet wired into `full_curve25519_amd64_64_ucode.c` or `full_curve25519_4x64.c`, so the end-to-end 4×64 X25519 figures still reflect v3 |
| 11 | "$5\times51$ leaves thirteen unused bits in each 64 bit limb" | arithmetic ($64-51$) | Confirmed |
| 12 | "The saturated representation … requires full width propagation … We therefore design the two implementations around different carry paths" | `asm_op_curve25519_solinas_mul.c` (SETCC path, 4×64); `_chained_v3.c`; `_chained_v4.c`; **`asm_op_curve25519_solinas_vs.c` (new, all three same process, pinned)** | **Overstated; now measured.** The saturated representation does not *require* path 2, and path 2 barely wins. Pinned run 2026-09-07 (TSC/core = 1.0000, turbo off, anchors: naive C 135 vs 132 reference, fiat 224 vs 224 exact), each arm verified 10,007/10,007, v3 and v4 cross-checked identical on 500 random pairs:<br>SETCC dance — 119 triads, **211** cyc/op<br>ADC chain (v3) — 75 triads, **207** cyc/op<br>ADC chain fused (v4) — 67 triads, **191** cyc/op<br>The two carry paths differ by 2%. The ADC path's 2-vs-3 µops per carry buys almost nothing; the 9.5% came from fusing the schoolbook into the chain |
| 13 | "rather than forcing the same microcode schedule onto both representations" | as above | **Now the opposite of the finding.** The schedule mattered far more than the path. Also note the saving did not track triad count: −44 triads (SETCC→v3) bought 4 cycles, −8 (v3→v4) bought 16, because those eight sat between each multiply and its consuming addition. Position on the dependency chain, not patch size, is what a triad costs — consistent with [[microcode_firing_latency]] (0 cyc interleaved, ~1+ serial) |
| 14 | closing comparison to $5\times51$ | same run | All three 4×64 versions lose to the 58-cyc 5×51 multiplication *and* to plain `-O3` C at the same radix (135 cyc). The honest boundary statement is the second one — it does not require a cross-representation comparison |

## Missing from the text

1. **Arch CF is frozen at hook entry.** Stated nowhere, yet it is the reason a
   bridge is needed at all, and the reason each carry hop needs its own
   `GENARITHFLAGS_RR` rather than one at the start of the chain.
2. **The op counts.** Path 1 costs 3 $\mu$ops per carry, path 2 costs 2. That
   is the entire quantitative point the subsection is building toward and it is
   never stated. (`additionwithflags.c` header states it.)
3. ~~**`SETCC`'s own destination must be a `TMP`**~~ — **this rule is false.**
   It is stated in CLAUDE.md ("SETCC_CONDB_DR only works on TMP registers, NOT
   arch registers") and in `simple/SUMMARY.md` line 135, and no probe anywhere
   in the tree had ever put a `SETCC` destination in an architectural register.
   `carrystate_scope.c` Group B does: `SETCC_CONDB_DR(RAX, TMP0)` returns the
   correct carry for carry/sum-0, carry/sum-1 and no-carry inputs (B1), and B2
   reads the *same* carry twice in one triad — once into `RAX`, once into
   `TMP1` — and both paths agree. The real constraint is the one in item #1:
   the register whose state is *read* must be a `TMP`. The destination need not
   be. **E1 settles the write width:** with the destination pre-filled with
   all-ones, `SETCC_CONDB_DR(RAX, TMP0)` yields exactly
   `0x0000000000000000` / `0x0000000000000001` — a full 64-bit zero-extending
   write, not a narrow one. So `SETCC → TMP; ZEROEXT → arch` collapses to a
   single `SETCC → arch` wherever the carry's only consumer is an
   architectural register.
   **Scope of that saving, measured, not assumed:** *zero* sites in the two
   patches this paper reports — every `ZEROEXT` from the `SETCC` destination in
   `asm_op_curve25519.c` and `asm_op_curve25519_mul.c` targets `TMP9`, the
   carry-sum register, not an architectural one. The only genuine sites in the
   tree are 9 in `asm_op_p521_sq.c` (one per limb section:
   `SETCC_CONDB_DR(TMP15, TMP13)` … `ZEROEXT_DSZ64_DR(R10, TMP15)`, commented
   "R10 = carry1", collapsible to `SETCC_CONDB_DR(R10, TMP13)`), and that patch
   already needs 18 fragments and loses to fiat-crypto. The `ZEROEXT`s from
   `SETCC` destinations in the Montgomery patches move limb values, not
   carries, and are not elidable. Treat this as a capability correction, not a
   performance result.
4. **State lifetime.** The per-destination carry persists until the next write
   to that register — a real scheduling constraint (`microcode_findings.md`
   §2c). Note the actual `additionwithflags.c` S8 observations, which are not
   what that file's hypothesis labels predicted: `ZEROEXT TMP0=7` after a
   carrying ADD returns **0**, i.e. it *clears* the latched carry (the file
   predicted "preserved"; this is the single exploratory probe that differed).
   `OR` also clears it. `SHL` of `2^63 << 1` returns 1, but that shift carries a
   bit out, so "preserved the ADD's carry" and "wrote its own carry" both
   predict 1 — undecided by that probe. **Group E2 decides it: every write
   replaces the register's condition state.** E2b (carry latched, then a shift
   that shifts nothing out) returns 0 — the ADD's carry is gone. E2c (no carry
   latched, then `2^63 << 1`) returns 1 — the shift published its own
   shifted-out bit. E2d (carry latched, then a `MUL` whose high half lands in
   the same register) returns 0. E2e control (`MUL` writes a different register)
   returns 1, the carry survives.

   Consequences: (a) the lifetime wording in `CLAUDE.md` and
   `microcode_findings.md` §4 — "persists until the next ADD/SUB to that same
   TMP" — is too permissive; any $\mu$op writing the register destroys the
   state, which is a stricter scheduling constraint than the one
   `microcode_findings.md` §2c tells patch authors to respect. (b)
   `EXPERIMENTS.md`'s "MUL before ADD+SETCC — **Safe:** MUL does NOT poison the
   internal flag domain" holds only because the MUL there writes other
   registers; `MUL` has *two* destinations (high half and `srcB`) and both
   replace their register's state. (c) `SHL` publishing its shifted-out bit as
   a readable carry is a primitive we have not exploited anywhere.
5. **Chain length and cost for path 2:** verified to 32 limbs, ~8 cyc/limb
   (`chain_length.c`, `chained_4limb_add.c`).
6. **Intra-triad packing** `{ADD/ADC, GENARITHFLAGS_RR(t,t), ADC}`
   (`test_adc_gfl_adc_intratriad.c`) — what took the 4×64 patch from 95 to 75
   triads and made path 2 competitive at all.
7. **The dead ends.** `MOVEINSERTFLGS`, `MOVEMERGEFLGS`, and all six
   `MOVETOCREG`/`CORE_CR_EFLAGS` variants fail to write ADC's carry-in
   (`test_adc_chain.c`, `test_movetocreg_eflags.c`, `cr_cf_probe.c`). This is
   what licenses "`GENARITHFLAGS_RR` is the bridge" as opposed to "the bridge we
   happened to find".
8. **Path 2's TMP-only constraint has a measured cost:** accumulators that live
   in arch registers across rows must be staged into TMPs and copied back with
   `ZEROEXT`, ~2 triads per row. That, plus the bridge $\mu$op, is why the
   op-count saving (3→2) did not translate into a speed-up until v3.

## Stale claims elsewhere that this recheck contradicts

- `paper/OUTLINE.md` line 125 and claim **C10**: "**ADC is unusable**; every
  carry chain must route through ADD→TMP + SETCC_CONDB." False since
  2026-05-22 — the 4×64 control is an end-to-end chained-ADC implementation
  that passes 4/4 RFC 7748. Fix before the outline is used as a claim ledger.
- `simple/microcode_findings.md` §4 ("the bridge that almost works") and §5
  ("we cannot reliably use ADC"), and §8 ("why `GENARITHFLAGS_R` fails
  sporadically"). The 1-arg form does not fail sporadically; it never bridged.
  Superseded by `loss-againstamd64-64.md` §11.
- `tests/adc_findings.md` — the 2026-04-17 document still states
  "GENARITHFLAGS is the bridge" and gives the 1-arg `GENARITHFLAGS_R(TMP0)`
  canonical 128-bit add. Superseded; it needs a header pointing at
  `loss-againstamd64-64.md` §3 and §11.
- `additionwithflags.c` header says the per-register carry is written "by every
  ADD / ADC to its destination register … each TMP/arch reg carries its own
  latched carry". The "arch" half is **wrong** — Group A shows arch destinations
  hold nothing queryable. Fix the header before citing the file.
- `CLAUDE.md` ("Hardware Constraints") and `simple/SUMMARY.md` line 135:
  "SETCC_CONDB_DR only works on TMP registers, NOT arch registers", and
  `CLAUDE.md` Lesson 1: "SETCC on arch registers fails silently — always
  ADD→TMP, SETCC→TMP, then ZEROEXT→arch". Group B disproves the destination
  half of both. The correct statement is: the register whose condition state is
  *read* must be a TMP; SETCC's destination may be architectural.
