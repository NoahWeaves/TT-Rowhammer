# Run Report — 2026‑04‑25 (Round 2, with ECC instrumentation)

Hardware: 4× Tenstorrent Blackhole, fw 19.4.2, GDDR6 @ 16 Gbps, all 8
channels enabled per chip (`enabled_gddr=0xff`, `ddr_status=0x5555`),
per‑chip max GDDR temp 44–52 °C.  All experiments on chip 0.

What changed since round 1:
- Probe kernel rewritten to use a **16‑cacheline (1 KiB) burst from row B**
  before timing the re‑access of A, and to take a **median over 33 trials**
  instead of a min of 16. ([kernels/geometry_probe_kernel.cpp](../kernels/geometry_probe_kernel.cpp))
- Scratch buffers in [verify_geometry.cpp](../experiments/verify_geometry.cpp) and
  [build_row_set.cpp](../experiments/build_row_set.cpp) bumped to 2 KiB.
- `pyluwen` enabled via `python_env`. Wrapper [ecc_hammer.py](../experiments/ecc_hammer.py)
  snapshots `gddr01..67_corr_errs` and `gddr_uncorr_errs` before/after a run.
  *(In practice the existing hammer binary already prints the same counters
  internally, so the wrapper is just a cross‑check; it agreed.)*

---

## Verdict per attack (round 2)

| ID | Attack | Total acts | Visible flips | ECC corr | ECC uncorr | Outcome |
|----|--------|-----------:|--------------:|---------:|-----------:|---------|
| B1 (re‑probe) | 8×8 conflict matrix, 16‑CL evict + median | n/a | n/a | n/a | n/a | ⚠️ STILL UNIFORM (451–459 cyc, no separation) |
| B2 (re‑probe) | k = 1..32 stride, 16‑CL evict + median | n/a | n/a | n/a | n/a | ⚠️ no jump detected |
| Heavy n‑sided | 8 rows × 14‑side × 4‑core × 5 M iter | 2.24 × 10⁹ | 0 | 0 | 0 | ❌ FAIL |
| Barrier mode  | 8 rows × 14‑side × 4‑core × 1 M iter `--barrier` | 4.48 × 10⁸ | 0 | 0 | 0 | ❌ FAIL |
| TRR sweep ch0 | num‑sides ∈ {2,4,6,8,10,12,14}, 8 rows × 2 M iter × 4 cores | ≈4.5 × 10⁹ across all sides | 0 | 0 | 0 | ❌ FAIL — **no break‑point found** |
| Pattern × channel grid | ch ∈ {0,4} × pat ∈ {0x00,0xFF,0x55,0xAA}, 4 rows × 14‑side × 5 M iter × 4 cores | ≈9.0 × 10⁹ | 0 | 0 | 0 | ❌ FAIL across 8 (ch, pattern) cells |

**Cumulative activations across this session: > 1.5 × 10¹⁰ (15 billion).
Visible bit flips: 0. ECC‑corrected events: 0. Uncorrectable events: 0.**

This is a strong negative result — every observable signal that any
activation is reaching the array as a discrete row open is zero.

---

## What the probe data tells us

After upgrading to a 16‑cacheline (1 KiB) burst‑evict + 33‑sample median,
**every pair on channel 0 still measures 451–459 cyc.** No separation
between same‑row, diff‑row, and cross‑bank‑group cases.

For comparison the TT‑Rowhammer reference matrix (same hardware family,
same probe pattern) shows 833 / 873 / 897 cyc. We are missing the entire
row‑activation cost. The two simplest explanations:

1. **Open‑page policy plus row‑buffer sharing.**  GDDR6 controllers on
   Blackhole appear to keep enough rows open per bank that a 1 KiB burst
   from a "B" row does not displace "A". The test would need either a
   *much* larger eviction set (e.g. iterate through every aggressor in the
   same bank) or a way to force an explicit precharge. Neither is reachable
   from BRISC user code without controller assistance.
2. **Wall‑clock register rate change.**  Even the same‑row baseline reads
   as 451 cyc here vs. 833 cyc in the reference, a 1.85× ratio that matches
   the ratio between a hypothetical doubled BRISC clock and the assumed
   800 MHz baseline. If true the *separation* of same vs diff still ought
   to scale, and we would expect ≈22 cyc of spread — not the 8 cyc we see.

Either way, the BRISC‑timed probe is not actionable on this hardware/firmware
stack and **C3 DRAMA discovery cannot be validated this way.** Going forward
we would need either Tracy zone counters or a controller‑level perf signal.

---

## What the ECC data tells us

This is the more important result. The hammer kernels themselves do
documented work:

- 14‑sided × 4 cores: ~52 M activations/s per row (matches `Summary_Main_Personal.md`).
- Barrier mode: ~5.6 M activations/s, fully serialized — guaranteed
  one‑activation‑per‑request semantics from the BRISC side.

At >10¹⁰ requests, both `gddr_corr_errs` and `gddr_uncorr_errs` stayed at
zero. There are only two plausible interpretations, both pointing at the
controller, not the BRISC kernel:

- **Activations are absorbed.** The DRAM controller serves repeat reads to
  the same row from the row buffer (or a higher‑level read combine), so
  each `noc_async_read` does not produce a fresh `ACT` to the array. With
  no actual activations there is nothing for ECC to catch.
- **TRR / RFM is succeeding silently.** Even if every request did issue an
  `ACT`, GDDR6 mandatory TRR would refresh victims before charge leakage
  becomes detectable. ECC would still be quiet because no bits flipped.

The TT‑Rowhammer documentation acknowledges this directly:
> "A clean run with no flips and no ECC deltas means either the part is
> fully mitigated at the chosen parameters, or the aggressor pattern is not
> yet strong enough."

Our cross‑sweep evidence (varying iterations 4 orders of magnitude, varying
sides 2..14, varying patterns, varying channels, both pipelined and
barrier‑serialized) makes the "not strong enough" branch unlikely from
the user side: the bottleneck is below BRISC.

---

## Concrete unblocks remaining (out of scope for this session)

These would require capabilities we do not have in user space:

1. **Controller perf counters.** Read the GDDR6 controller's per‑channel
   ACT / RD / PRE counts via `axi_read32` over an undocumented register
   block. That answers "are activations actually firing?".
2. **Disable on‑die ECC** (or read raw via the test mode). If there is no
   signed test mode this is genuinely impossible.
3. **TRR table overflow.** GPUHammer used > 2× the TRR table size; on
   GDDR6 that is typically up to 16 sides. We tested up to 14 — would need
   bigger MAX_AGGR in the n‑sided kernel and matching driver work.
4. **Controller‑level row mapping leak.** Inspect FW init logs / SPI
   ROM tables for the actual physical→DRAM mapping the controller uses.

---

## Files written / updated this session

| Path | Change |
|------|--------|
| [kernels/geometry_probe_kernel.cpp](../kernels/geometry_probe_kernel.cpp) | Burst eviction (16 CL) + median timing |
| [experiments/verify_geometry.cpp](../experiments/verify_geometry.cpp) | Scratch bump |
| [experiments/build_row_set.cpp](../experiments/build_row_set.cpp) | Scratch bump |
| [experiments/ecc_hammer.py](../experiments/ecc_hammer.py) | New — pyluwen ECC wrapper |
| [verification_report.md](verification_report.md) | Auto‑updated with stronger probe data |
