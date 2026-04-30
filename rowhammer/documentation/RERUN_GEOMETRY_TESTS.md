# Rerun Plan: Bank-Size and Same-Bank Discovery Probes

You are picking up an unfinished thread. Read this top to bottom before
running anything.

---

## What you're inheriting

The 2026-02 characterization run pinned the **row-level** geometry of
Blackhole's GDDR6 cleanly: row size 8 KB, sequential mapping, no XOR,
hit/miss latencies 833/873 cyc, plus a third tier at 897 cyc that's
inferred to be cross-bank-group. That data lives in `address_bit_mapping.txt`,
`row_size_determination.txt`, `row_verification.txt`, `conflict_matrix.csv`.

The **bank-level** geometry is *inferred*, not measured:
- `ROWS_PER_BANK = 16` is hardcoded in two places, derived from "the
  bit-toggle chart's 873→897 jump happens at bit 17, so 2⁴ = 16 rows
  per bank." This drives `select_same_bank_aggressors()` in the n-sided
  hammer. If wrong, n-sided hammers attack the wrong rows.
- `experiments/build_row_set.cpp` (DRAMA-style) and `experiments/verify_geometry.cpp`
  (Phase B2) were both built specifically to *directly* measure this.

Both probes have been executed. Both produced **unusable output** —
every cell flatlines at ~451 cyc instead of separating into the
833/873/897 bands. See `documentation/verification_report.md` and
`experiments/results/row_set_ch0_anchor1024.csv`. The C3 row-set output
file is empty (zero same-bank rows discovered) as a result.

Your job: get a clean re-run.

---

## Why the existing data is useless (don't skip this)

A single ~451 cyc reading across every probe means the BRISC
wall-clock side-channel is not producing the signal the kernel was
written against. Either the timer source, the eviction burst, or the
kernel processor mapping changed between the original 2026-02 run and
when these follow-ups were executed.

**Do not run the bank-size or row-set probes until you've confirmed the
833/873/897 tiers can be reproduced.** Otherwise you will reproduce the
existing flatlined output.

---

## Hardware / build prerequisites

- Tenstorrent Blackhole card visible to `tt-smi` (`tt-smi -ls`)
- `tt-metal` SDK at `~/tt-metal`, with `TT_METAL_HOME` exported
- `cmake`, `clang`, Python 3.8+ with `numpy`, `matplotlib`
- Working directory for builds: `~/tt-metal`

---

## Phase 0 — Confirm the timer is healthy

The single most important step. If this doesn't pass, fix it before
moving on.

```bash
cd ~/tt-metal
cmake --build build --target metal_example_validation_test -j$(nproc)
./build/programming_examples/metal_example_validation_test
```

**Pass criteria:**
- Stdout ends with `ALL TESTS PASSED (401/401)`.
- Method 2 (8 KB conflict matrix) shows diagonal ≈ 833 cyc, off-diagonal ≈ 873 cyc.
- Method 5 (bit toggle) shows three bands: 833 (bits 6–12), 873 (bits 13–16), 897 (bits 17–25).

**If you see ~451 cyc anywhere — STOP and diagnose.** Likely causes,
in rough probability order:
1. The probe kernel's `DataMovementProcessor` / `NOC` config differs from
   the canonical `tt_metal/programming_examples/dram_latency/kernels/validation_probe.cpp`.
   Diff `rowhammer/kernels/geometry_probe_kernel.cpp` host-side launch
   args (`verify_geometry.cpp`, `build_row_set.cpp`) against the
   canonical `validation_test.cpp` host launch.
2. BRISC AICLK is not 800 MHz — check `tt-smi -d` for clock state.
3. `RISCV_DEBUG_REG_WALL_CLOCK_{L,H}` register address moved between
   SDK versions. Grep the SDK headers; the canonical kernel works, so
   compare include paths.
4. The eviction burst (`EVICT_BURST = 16` cachelines) isn't actually
   forcing a row precharge — possible if the kernel runs on a core/NOC
   path that hits a different DRAM tile sub-port.

Do not proceed past Phase 0 until 833/873 reproduces.

---

## Phase 1 — Direct measurement of ROWS_PER_BANK

```bash
cd ~/tt-metal
cmake --build build --target verify_geometry -j$(nproc)
./build/programming_examples/rowhammer/verify_geometry --all-channels
```

**What it does (per channel):**
- B1: 8×8 conflict matrix at 8 KB stride. Sanity check.
- B2: Pairs `(anchor, anchor + k·8KB)` for k = 1..32. Looks for the
  first k where latency steps from 873 to 897.

**Pass criteria:**
- B1: every diagonal ≤ 858 cyc, every off-diagonal ≥ 848 cyc.
- B2: a clean step from ~873 to ~897 at some k. Hypothesized: k = 16.

**Output:** overwrites `rowhammer/documentation/verification_report.md`.

**Action items based on result:**

| Result | What to do |
|---|---|
| Step at k = 16 on all 8 channels | Confirms the inferred constant. Document and proceed to Phase 2. |
| Step at k ≠ 16 (consistent across channels) | Update `rowhammer/experiments/dram_addr_decoder.hpp:31` (`BANK_GROUP_SHIFT`) and `rowhammer/rowhammer_nsided.cpp:126` (`ROWS_PER_BANK`). Re-build n-sided. |
| Step inconsistent across channels | Surface in the report. Do not change the constant. Halt and ask the user. |
| No step found in k ≤ 32 | Bump the sweep to k = 1..64 in `verify_geometry.cpp:175`. Re-run. If still nothing, the bit-17 inference may be wrong; halt. |

**Caveat in the existing harness:** the exit gate logic in
`verify_geometry.cpp:268-274` treats "no jump found" as a warning, not
a failure, and lets the report claim success. Fix this before relying
on the report's verdict — change the verdict logic so that
`r.b2_measured_rows_per_bank == 0` sets `b2_any_fail = true`.

---

## Phase 2 — DRAMA-style same-bank discovery

Only run this after Phase 1 passes.

```bash
cd ~/tt-metal
cmake --build build --target metal_example_rh_build_row_set -j$(nproc)
./build_Release/programming_examples/metal_example_rh_build_row_set \
    --channel 0 --anchor-row 1024 --sweep 256
```

(Build directory may be `build` or `build_Release` depending on the
preset; check what `cmake` produced.)

**What it does:** for anchor A, times `(A, A + k·8KB)` for k = 1..256,
classifies each by latency tier, writes the same-bank set to disk.

**Pass criteria:**
- ~1 in `ROWS_PER_BANK` of the 256 rows should land in the SAME_BANK
  tier (873 ± 25 cyc). Not zero. Not all of them.
- The same-bank rows should match the predicted set
  `{k : k % ROWS_PER_BANK ∈ same-bank-positions}` from the assumed
  model. Disagreement is the high-value finding — surface it.

**Output:**
- `rowhammer/experiments/results/row_set_ch0_anchor1024.csv` — full sweep
- `rowhammer/experiments/results/row_set_ch0_anchor1024.txt` — same-bank offsets only

**Pre-flight fix worth doing:** the SAME_BANK tier band in
`build_row_set.cpp` is hardcoded as `851..890` cyc, which is
*narrower* than the validated `873 ± 25 = 848..898` band used elsewhere.
A real measurement at 850 or 891 cyc would be silently dropped. Widen
to match the verification report's tolerances before running.

**Repeat for at least one anchor on each of the 8 channels** if Phase 1
revealed any per-channel variation. If channels are identical, three
anchors (low, mid, high) on channel 0 are enough.

---

## Phase 3 — Document the outcome

1. **Verification report.** `verify_geometry` overwrites
   `documentation/verification_report.md` automatically. Add a manual
   Phase B3 / B4 section describing what you did and any constant
   updates.

2. **New run report.** Create `documentation/RUN_REPORT_R4.md`
   following the structure of `RUN_REPORT_R3.md`. Include:
   - Date, hardware, SDK commit hash
   - Phase 0 output (validation pass/fail)
   - Phase 1 result (B1 + B2 per channel)
   - Phase 2 result (same-bank discovery vs. assumed model)
   - Whether `ROWS_PER_BANK` was changed and the new value
   - Any divergence between channels

3. **Update the status table.** If `ROWS_PER_BANK` is now directly
   measured, change it from `inferred` to `confirmed` in
   `generate_geometry_status_table.py:14`.

---

## Things to ask the user before doing, not after

- Anything that changes `ROWS_PER_BANK` in committed code — confirm the
  measurement first, then propose the change with the data.
- Modifying the verdict logic in `verify_geometry.cpp` — small, but it
  changes whether reports claim "PASS" or not.
- Running anything that takes more than ~10 seconds of card time
  (full Phase 2 across all 8 channels at multiple anchors).

## Things you can do without asking

- Phase 0 and Phase 1 single-channel runs (fast, read-only, no state
  change to the card).
- Diffing kernel configs to diagnose the 451 cyc issue.
- Widening the SAME_BANK tier band in `build_row_set.cpp` — it's a
  bug fix.

---

## Success exit criteria

- Phase 0 reproduces 833/873/897.
- Phase 1 produces a clean k where the 873→897 step occurs, and
  `ROWS_PER_BANK` is either confirmed or updated based on it.
- Phase 2 produces a non-empty same-bank set on at least one anchor,
  and its membership pattern is compared to the assumed
  `±k·0x2000` model.
- `RUN_REPORT_R4.md` exists with the above written down.
