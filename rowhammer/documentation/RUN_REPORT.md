# Phase B + C Run Report — 2026‑04‑25

Hardware: 4× Tenstorrent Blackhole (firmware bundle 19.4.2, KMD 2.6.0).  All
runs on **chip 0**, worker core `(1,2)`.  Reference baseline data from
`TT-Rowhammer/conflict_matrix.csv` (median 833 / 873 / 897 cyc bands).

Results CSVs are under [experiments/results/](../experiments/results/).

---

## Verdict per attack

| ID | Attack | Outcome | Bit flips | Notes |
|----|--------|---------|-----------|-------|
| B5 | Address decoder unit test | ✅ PASS | n/a | All 12 reference pairs decoded correctly. |
| B1 | 8 KB conflict matrix | ⚠️ INCONCLUSIVE | n/a | Probe kernel returns uniform 451 cyc for every pair. |
| B2 | Cross‑bank‑group row jump | ⚠️ INCONCLUSIVE | n/a | Same uniform 451 cyc → no jump detected. |
| C3 | DRAMA row‑set discovery | ⚠️ INCONCLUSIVE | n/a | All 256 candidate offsets classify as `SAME_ROW` (uniform 451 cyc). 0 % agreement with assumed `ROWS_PER_BANK=16`. |
| Baseline 1S/2S | `metal_example_rowhammer` ch0, 8 rows × 400k acts | ❌ FAIL (no flips) | 0 | 29.6 M act/s, ECC counters unavailable (`pyluwen` not in PATH). |
| C1 | REF‑sync hammer, 33 delay points × 1 M acts | ❌ FAIL (no flips) | 0 | Rate decays monotonically 28.6 → 1.0 M act/s; **no plateau** observed. |
| C2 | Pattern sweep, 4 rows × 49 (victim,aggressor) combos | ❌ FAIL (no flips) | 0 | All 196 launches at 500k iters returned 0. |
| n‑sided heavy | 14‑sided × 4‑core × 32 rows × 5 M acts | ❌ FAIL (no flips) | 0 | 280 M acts/row at 52 M act/s. |
| n‑sided extreme | 14‑sided × 8‑core × 4 rows × 50 M acts (pat=0) | ❌ FAIL (no flips) | 0 | ≈5.6 G activations per row. |
| All‑channel scan | 8 channels × 4 rows × 1 M acts × 8‑sided | ❌ FAIL (no flips) | 0 | All 8 GDDR6 channels produced zero visible flips. |

**Bottom line:** Across every technique implemented (single‑sided, 2‑sided,
14‑sided × 8‑core, REF‑sync delay sweep, 7×7 pattern sweep, pattern 0x0/0x55/0xFF
variants, 8/8 channels), **zero visible bit flips were produced** on this
Blackhole GDDR6 part.  The strongest run delivered ≈5.6 × 10⁹ activations per
victim row without effect.

---

## Why every technique reported zero

### 1. Probe kernel can't distinguish row states
Every pair in B1, B2 and C3 returned the same 451 cyc figure.  That number is
plausible for a single 64‑byte `noc_async_read` whose round‑trip is dominated
by NoC transit (4 hops × ~100 cyc ≈ 400 cyc) — it is **below** the 833 cyc
"same‑row hit" reference, which means the timed re‑read of A is *not*
reaching the DRAM array at all.  Most likely the GDDR6 controller is keeping
A's row buffer open across the intervening B access (per‑bank row buffer
hit), so the warm‑up sequence never forces an activation.  The TT‑Rowhammer
reference matrix used a richer timing methodology (median over many longer
bursts) that we did not reproduce.

Implication: we cannot empirically validate the assumed
`ROWS_PER_BANK=16` mapping with the current probe kernel.  Until the probe
is reworked (e.g. multiple back‑to‑back B accesses to force eviction, or a
PERF‑counter‑based measurement), C3‑style DRAMA discovery is unusable on
this stack.

### 2. The hammer attacks themselves landed on a refresh‑hardened part
All hammer launches succeeded and posted ~30–52 M activations/s — the kernel
is doing real work, the rates match the TT‑Rowhammer characterization
exactly.  But:

* GDDR6 has **on‑die ECC** plus an opaque controller refresh policy.  The
  vendor telemetry interface (`pyluwen`) wasn't on PATH so we could not
  read `gddr01..67_corr_errs` to see if invisible single‑bit corrections
  fired.
* No flips at 5.6 × 10⁹ activations/row exceeds typical DDR4 hammer
  thresholds (10⁴–10⁵) by 4–5 orders of magnitude.  Either the row‑pair
  geometry we hammered does **not** put aggressors and victim in the same
  bank/sub‑array (very plausible — see #1, the model isn't validated), or
  the part has TRR/RFM aggressive enough to refresh victims before any
  charge leakage accumulates.

### 3. C1 REF‑sync delay sweep showed no plateau
GPUHammer sees a flat plateau where the inserted delay aligns with tREFI
boundaries.  We saw a perfectly monotonic drop from 28.6 → 1.0 M act/s as
delay grew — i.e. the delay loop is purely additive overhead with no
controller‑level resonance.  That is consistent with the controller not
exposing tREFI‑bound stalls to user kernels (so we can't time‑align with
REF from BRISC alone).

---

## Required next steps before we can claim "TT is rowhammer‑resistant"

1. **Fix the probe kernel.**  Replace the single‑shot warm sequence with
   either (a) a long burst of B accesses (e.g. 64 cachelines) before timing
   A, or (b) use the BRISC NoC perf counters / Tracy zone counts to measure
   work rather than wall clock.  Without a working probe we have **no
   ground truth** that aggressors and victim share a bank, which is a
   prerequisite for any rowhammer technique.
2. **Wire `pyluwen` into PATH** (or the equivalent SMI library) so the
   campaign script can sample `gddr_corr_errs` before/after each row.  ECC
   correction events are the canonical evidence that activations *do*
   reach the array even if no visible flip leaks out.
3. **Try address pairs from the existing TT‑Rowhammer same‑bank list** at
   `TT-Rowhammer/row_verification.txt` *exactly verbatim* (instead of
   computing them from `dram_addr_decoder.hpp`) — this isolates whether
   the failure is "wrong addresses" vs. "hardware just won't flip".
4. **Run the manual B3 step** (`metal_example_rowhammer --barrier`) and
   capture pyluwen counters, to separate ECC‑corrected hammer from
   genuinely silent DRAM.

---

## Files written this session

- [results/find_trefi_ch0_row1024.csv](../experiments/results/find_trefi_ch0_row1024.csv)
- [results/pattern_sweep_ch0.csv](../experiments/results/pattern_sweep_ch0.csv)
- [results/row_set_ch0_anchor1024.csv](../experiments/results/row_set_ch0_anchor1024.csv)
- [results/row_set_ch0_anchor1024.txt](../experiments/results/row_set_ch0_anchor1024.txt)
- [verification_report.md](verification_report.md) (B1/B2 raw cycle table)
