# Rowhammer on Tenstorrent — Implementation Plan

Companion to [`CONTEXT.md`](CONTEXT.md). Read that first.

The plan has three phases:

- **Phase A — Documentation** (this file + `CONTEXT.md`).
- **Phase B — Verification** of everything we currently believe, before
  building anything new on top of it.
- **Phase C — New attack techniques**, in priority order.

> **Hard rule:** no Phase C work begins until the Phase B verification report
> is green or its discrepancies are explicitly accepted in writing. This is
> the "verify first" task you asked for.

---

## 1. What we have done & tried

A condensed checklist; see [`CONTEXT.md`](CONTEXT.md) for the full evidence.

### 1.1 DRAM reverse-engineering (TT-Rowhammer repo)
- Built a BRISC-side timing primitive using `RISCV_DEBUG_REG_WALL_CLOCK_L/H`
  and pipelined `noc_async_read` of 64 B cache lines.
- Determined **row size = 8192 B** via 5 independent methods (boundary pairs,
  8 KB conflict matrix, sub-row 1 KB matrix, stride sweep, address-bit
  toggle), 401/401 PASS.
- Showed **no XOR address interleaving** on bits 6–25.
- Identified **two latency tiers**: row-buffer miss (873 cyc) and
  cross-bank-group (897 cyc) — used to *infer* (not measure)
  `ROWS_PER_BANK = 16`, i.e. 128 KB per bank.
- Verified geometry is **identical across all 8 GDDR6 channels** at NOC
  endpoints `(0,1)(0,10)(0,4)(0,7)(9,1)(9,10)(9,4)(9,7)`.
- Established the address-construction primitives in
  [`CONTEXT.md` §2](CONTEXT.md#address-layout) so any host buffer offset can
  be mapped to a DRAM row in O(1).

### 1.2 Hammer infrastructure (this folder)
- Built a **double-sided pipelined hammer** ([`rowhammer.cpp`](../rowhammer.cpp)
  + [`kernels/rowhammer_kernel.cpp`](../kernels/rowhammer_kernel.cpp)) that
  alternates reads to `victim ± 0x2000` with no barrier inside the loop
  → **~14–17 M activations/s** (compared to ~1.8 M/s for the serialized
  variant kept as a control behind `--barrier`).
- Built an **n-sided + multi-core hammer**
  ([`rowhammer_nsided.cpp`](../rowhammer_nsided.cpp) +
  [`kernels/rowhammer_nsided_kernel.cpp`](../kernels/rowhammer_nsided_kernel.cpp))
  that round-robins up to 15 same-bank aggressors per core with up to 4
  parallel Tensix workers, intended to overflow any in-DRAM TRR-style
  tracker.
- Three-phase kernel structure: write known pattern → tight hammer loop →
  read back & compare; first 32 flipped words logged in detail (cache-line
  index, word offset, expected, actual).
- Safety: skip rows below the allocator base + 2 to avoid stomping on
  firmware/profiler memory.

### 1.3 ECC evidence (how we know it exists, despite no flips on readback)
- Both host drivers read **pyluwen** telemetry counters
  (`gddr01_corr_errs` … `gddr67_corr_errs`, `gddr_uncorr_errs`) before, every
  N rows during, and after the run.
- When a hammer pass leaves `corr_errs` incremented but readback shows zero
  flips, the driver explicitly prints
  `*** ECC IS MASKING ROWHAMMER BIT FLIPS ***`. That counter delta is our
  evidence that flips occur physically and that on-die SEC ECC is silently
  correcting them.
- We have **not** found a documented user/firmware toggle to disable on-die
  GDDR6 ECC on Blackhole.

### 1.4 What we have *tried* but has *not* yielded a flip yet
- Double-sided hammer at 500 K iterations on 64 rows of channel 0.
- N-sided (up to 14 same-bank aggressors) on 4 cores at 5 M iterations.
- Multiple data patterns are **supported** (CLI `--pattern`) but a systematic
  pattern sweep has not been run.

---

## 2. What is missing (relative to GPUHammer & the wider literature)

| # | Missing piece                                                                  | Source             | Why it matters |
|---|--------------------------------------------------------------------------------|--------------------|----------------|
| 1 | **Refresh-synchronized hammering** (per-loop calibrated delays).               | GPUHammer Step 3, BlackSmith, SMASH | All published GPU and recent CPU successes need REF alignment to defeat in-DRAM mitigations. |
| 2 | **Empirically-discovered same-bank address sets** (DRAMA-style row sets).      | GPUHammer Step 1   | We currently *assume* `victim ± k·0x2000` hits the same bank. Unverified. |
| 3 | **ECC bypass via multi-bit flips per code word**.                              | ECCploit           | Only realistic path to a *visible* flip given on-die SEC ECC. |
| 4 | **Aggressor data-pattern sweep** (solid-1, solid-0, striped, random-fixed).    | Kim et al. 2014    | Different patterns flip different cells; current code uses one pattern. |
| 5 | **Temperature control** (pre-heat package; log GDDR temp during run).          | Mutlu retrospective | Vulnerability strongly temperature-dependent. |
| 6 | **NOC sub-port parallelism** (3 sub-ports per channel are exposed).            | Blackhole arch     | We currently drive a single endpoint per channel; potentially 3× throughput. |
| 7 | **Cross-bank parallelism per core** (each core hammers a different bank).      | GPUHammer Fig. 4   | Avoids serializing through a single bank's command queue. |
| 8 | **Compiler-fold audit** of the BRISC hammer loop ELF.                          | GPUHammer "Compiler Settings Note" | A folded loop silently does no activations. |
| 9 | **`ROWS_PER_BANK` actually measured**, not inferred from a latency-tier jump.  | own gap            | Drives every n-sided pattern; if wrong, we hammer the wrong rows. |
| 10 | **A row-set / bit-flip exploit primitive** (move target data into a vulnerable row). | GPUHammer Step 4 | Out of scope until step 3 succeeds at least once. |

---

## 3. Verification task (Phase B) — run BEFORE any new technique

Goal: re-prove every prior claim with current hardware and code, so we never
build on stale numbers.

Output: **`documentation/verification_report.md`** with a ✅/⚠️/❌ table per
claim, current measured values next to the reference values from the
TT-Rowhammer artifacts.

### B1 — Characterization replay  *(parallelizable across channels)*
- Build a small "geometry probe" kernel that reproduces the three strongest
  TT-Rowhammer methods on the *currently installed* card:
  - Method 2: 8 KB conflict matrix (8×8).
  - Method 3: stride sweep 64 B → 12 KB.
  - Method 5: address-bit toggle on bits 6–25.
- Run on all 8 channels.
- **Pass** if median latency on each channel is within ±5 cycles of the
  reference values in [TT-Rowhammer/conflict_matrix.csv](../../../../../TT-Rowhammer/conflict_matrix.csv)
  (warn-only on >10 % divergence; do not block).

### B2 — `ROWS_PER_BANK` measurement  *(blocks Phase C)*
- Stride-sweep the same victim address paired with `victim + k·0x2000` for
  `k = 1..32` and time row-buffer-miss latency. Find the `k` at which median
  latency jumps from 873 → 897 cyc (cross-bank-group) — that is `ROWS_PER_BANK`.
- **Hard-fail** the report if measured value differs from the hardcoded `16`.
- If different, update the constant in
  [`kernels/rowhammer_nsided_kernel.cpp`](../kernels/rowhammer_nsided_kernel.cpp)
  and [`rowhammer_nsided.cpp`](../rowhammer_nsided.cpp).

### B3 — ECC baseline replay
- Run [`rowhammer.cpp`](../rowhammer.cpp) with `--hammer-iterations 0` and
  confirm pyluwen counters are stable across 30 s of polling.
- Run a known 500 K-iteration double-sided pass on channel 0, rows 32–95, and
  confirm `corr_errs` increments (this is our positive control for "ECC
  exists and is being exercised").

### B4 — Compiler-fold sanity
- Disassemble the produced BRISC ELF (path under
  `TT-Rowhammer/generated/watcher/`) for the hammer kernel.
- Grep for the issue points of `noc_async_read` inside the hammer loop body
  and confirm there are exactly 2 (double-sided) or `num_sides` (n-sided).

### B5 — Address-decoder unit test
- Add a header-only `dram_addr_decoder.hpp` that exposes
  `row(addr)`, `column(addr)`, `byte_offset(addr)`, `same_row`, and
  `same_bank_group` based on the bit layout in
  [`CONTEXT.md` §2](CONTEXT.md#address-layout).
- Unit-test it against every pair listed in
  [TT-Rowhammer/row_verification.txt](../../../../../TT-Rowhammer/row_verification.txt).

### Exit criteria for Phase B
- `verification_report.md` checked in.
- B1: ≥7/8 channels within ±5 cyc.
- B2: measurement either confirms 16 or constant updated and re-run shows
  consistent same-bank latency.
- B3: counter delta observed.
- B4: loop body intact.
- B5: 100 % of recorded pairs decoded correctly.

---

## 4. New techniques to try (Phase C)

In strict priority order. Each step adds files only under
[`kernels/`](../kernels) and a new `experiments/` subfolder; no rewrite of
`rowhammer.cpp` / `rowhammer_nsided.cpp` is required (they remain the stable
control baselines).

### C1 — REF-synchronized double-sided hammer  *(highest leverage)*
- New kernel `kernels/rowhammer_refsync_kernel.cpp` based on the existing
  double-sided kernel, with a per-loop calibrated delay implemented as a
  fixed `nop`/`add` chain (avoid `__syncthreads`-equivalent reordering, just
  like GPUHammer).
- Calibration host: `experiments/find_trefi.cpp` — sweeps the delay from 0
  to ~2000 BRISC cycles, plots achieved activation rate vs. delay, looks for
  the synchronization plateau (mirrors GPUHammer Fig. 5).
- **Success metric:** plateau is visible *and* sustained activation rate
  ≥14 M acts/s with the synchronizing delay inserted.

### C2 — Aggressor data-pattern sweep
- No new kernel — extend the existing kernel's runtime args to take an
  `aggressor_pattern` independent from `victim_pattern`.
- Host driver iterates over: `(victim, aggressor)` ∈
  `{0x00, 0xFF, 0x55, 0xAA, row-id-seeded, fixed-random}`².
- **Success metric:** flip rate per pattern combination logged to a CSV.

### C3 — Empirical same-bank row sets (DRAMA-style)  *(builds on B2)*
- `experiments/build_row_set.cpp`: for a chosen reference address, time
  every address in a 2 MB region at 64 B stride; classify same-bank by
  row-buffer-miss latency tier (873 ± δ).
- Refactor `select_same_bank_aggressors()` in
  [`rowhammer_nsided.cpp`](../rowhammer_nsided.cpp) to consume the discovered
  set instead of computing `victim + k·0x2000`.
- **Success metric:** ≥90 % of slots in the discovered set agree with the
  predicted `±k·0x2000` set (validates assumed geometry); disagreement is
  itself a high-value finding.

### C4 — Throughput max
- Combine C1 + C3 + per-core different-bank assignment + sweep the 3 NOC
  sub-ports per channel exposed by `blackhole_140_arch.yaml`.
- **Success metric:** total activations/s / channel exceeds single-core
  baseline by ≥2× without dropping per-row activation count below TRH
  threshold (~12 K from GPUHammer).

### C5 — Multi-bit-flip ECC bypass attempt  *(the exit criterion for the project)*
- Use C1 + 24-sided pattern from C3 + C2's worst-case patterns.
- **Success metric:** any nonzero `gddr_uncorr_errs` delta in pyluwen *or*
  any readback flip. Either result is project success.

### C6 — Temperature loop
- Pre-heat the package by running a sustained matmul on an unrelated set of
  cores to push GDDR temperature above 60 °C (read with pyluwen telemetry).
- Re-run C1, C5 at elevated temperature.
- **Success metric:** flip rate strictly increases with temperature, or new
  flips appear.

---

## 5. Open decisions

These should be answered before starting Phase C; defaults are recommended in
parentheses.

1. **Kernel folder layout** — flat `kernels/` (current style, *recommended*)
   vs. `kernels/refsync/`, `kernels/nsided/` subfolders.
2. **Open a Tenstorrent ticket** asking whether on-die GDDR6 ECC has any
   tt-smi/firmware toggle, in parallel with C1/C2 work? (*Recommended: yes.*)
3. **B1 strictness** — warn-only on >10 % divergence (*recommended*) vs. hard
   fail. B2 is hard-fail regardless.

---

## 6. Out of scope

- Model-weight tampering exploit (GPUHammer Step 4 / Hong et al. ICML19)
  until we have at least one observed bit flip.
- HBM and non-Blackhole Tenstorrent cards.
- Disabling ECC at firmware level — pursue bypass via multi-bit instead.
