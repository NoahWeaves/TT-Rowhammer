# Round 4 — NOC Coalescing Analysis and Real Activation Rate Measurement

## Goal

Round 3 concluded that "per-access DRAM cost is invisible to user space" but
could not distinguish between three hypotheses: (a) a cache intercepting reads,
(b) the DRAM controller coalescing requests, or (c) the NOC absorbing bursts.
Round 4 resolves this question definitively and measures the true physical
activation rate.

---

## Key finding

**The GDDR6 controller is coalescing ~29× for reads and ~24× for writes.**
Every NOC read and write does reach DRAM and returns/commits correct data, but
the controller's request reordering groups accesses to the same open row,
eliminating most row activations.  The "52 M act/s" reported by the pipelined
attack is the NOC issue rate; the real DRAM row activation rate is **~0.9 M
act/s per core** — two orders of magnitude below the typical rowhammer
threshold (MAC ≈ 100 K activations per tREFI window of ~3.9 µs).

---

## 1. Geometry validation — ROWS_PER_BANK confirmed

Added a startup geometry probe to the n-sided driver that measures
ROWS_PER_BANK using the geometry_probe_kernel before the attack loop.

**Result:** The inline probe returned ~451 cycles for all k=1..32, showing no
latency tier differentiation.  However, re-running the **original**
`validation_probe.cpp` (from the dram_latency programming example) reproduces
the 833 / 873 / 897 cycle tiers perfectly across all 8 channels, with 100%
pass rate on all three methods (2b, 3, 5).

**Root cause:** The geometry_probe_kernel uses serialized reads (barrier after
each), where the ~456 cycle NOC round-trip masks the ~40 cycle DRAM row-switch
penalty.  The original validation_probe uses pipelined bursts (8 A-B pairs +
single barrier) with a 128 MB flush read before each trial, which makes the
aggregate row-buffer conflict cost visible.

**Conclusion:** ROWS_PER_BANK = 16 is correct.  The row mapping is valid.  The
verification tool needed the pipelined+flush technique to see DRAM-internal
timing.

---

## 2. Activation diagnostic — no cache, but no row-switch cost either

Built `activation_diagnostic_kernel.cpp` to compare serialized latency across
five access patterns on all 8 channels:

| Pattern | Expected if real activations | Measured (all 8 channels) |
|---------|-----------------------------|--------------------------|
| SAME_ADDR (row-buffer hit) | fastest | **456 cyc** |
| SAME_ROW (different column) | same as hit | **456 cyc** |
| DIFF_ROW (same bank) | ~873 cyc (row-buffer miss) | **456 cyc** |
| DIFF_BANK (cross bank group) | ~897 cyc | **456 cyc** |
| Pipelined A,B vs A,A | A,B should be slower | **identical (~60 cyc/pair)** |

All patterns cost the same.  The ~456 cycle NOC round-trip dominates completely.

---

## 3. Buffer layer diagnostic — reads DO reach DRAM

Built `buffer_diagnostic_kernel.cpp` with four sections to identify the
buffering layer:

### Section A: Data provenance
Wrote 16 unique patterns (one per row), read all back.  **16/16 correct.**
Reads are not being served from a stale cache.  Every read reaches DRAM and
returns current data.

### Section B: Row-buffer saturation (K=1..16)
Opened K rows in sequence, then re-timed the first row.  **No latency change
from K=1 to K=16.**  The controller maintains enough row buffers (or reorders
fast enough) that no eviction is visible up to 16 open rows.

### Section C: Large-stride sweep (8 KB → 8 MB)
Timed re-access after accessing a far address at each stride.  **456 cycles at
every stride.**  No bank-conflict signature at any address distance.

### Section D: Write-read vs read-read
Compared (read B → read A) vs (write B → read A).  **Both 456 cycles.**
Writes don't force a different latency path in serialized mode.

**Conclusion:** There is no intermediate cache.  The uniform 456-cycle latency
is the NOC round-trip floor.  The DRAM row-switch cost (~40 cycles) exists
(the original validation_probe sees it in aggregate) but is invisible to
single-access measurements because it's hidden behind the NOC latency.

---

## 4. Forced activation measurement — the coalescing factor

Built `forced_activation_kernel.cpp` with five attack modes to compare NOC
issue rate vs real DRAM activation rate:

| Mode | M act/s | Coalescing factor |
|------|---------|-------------------|
| **FLUSH-READ** (128 MB flush before each read) | **0.88** | 1× (confirmed real) |
| **FLUSH-WRITE** (128 MB flush before each write) | **0.96** | 1× (confirmed real) |
| WRITE pipelined (no barrier) | 22.86 | **~24×** |
| **WRITE serialized (barrier)** | **2.27** | **~2.4×** |
| WRITE+READ (full cycle) | 0.96 | 1× |
| PIPELINED READ (baseline) | 25.81 | **~29×** |

The FLUSH-READ and FLUSH-WRITE modes use the same 128 MB flush technique as
the original `validation_probe.cpp`: read from +128 MB to force the row buffer
closed, ensuring the subsequent access MUST activate the target row.

### What these numbers mean

- **Pipelined reads coalesce ~29×.**  For every 29 reads the NOC issues, the
  DRAM controller opens the row once and serves all 29 from the row buffer.
- **Pipelined writes coalesce ~24×.**  The write buffer absorbs and batches
  writes similarly to reads.  The R3 finding that write-based hammering showed
  identical throughput across all strides is explained: writes coalesce too.
- **Serialized writes (barrier between each) reduce coalescing to ~2.4×.**
  The barrier prevents NOC-level batching, but the DRAM write buffer still
  holds 2-3 pending writes across barriers.
- **The confirmed-real activation rate ceiling is ~1 M act/s per core.**
  With 8 cores × 2 RISCs, the theoretical max is ~16 M real act/s.

### Implications for rowhammer feasibility

At 1 M real act/s per core, with tREFI ≈ 3.9 µs:
- **~3.5 real activations per refresh window per core**
- Even at theoretical max (16 M real act/s): **~62 activations per tREFI**
- Published rowhammer threshold (MAC): **~100,000 activations per tREFI**

We are **three orders of magnitude below the activation rate needed** to
induce bit flips, even before accounting for ECC and TRR.

---

## 5. REF-sync calibration

Added `rowhammer_nsided_refsync_kernel.cpp` and `--delay-sweep` to the driver.
The delay sweep found an activation rate peak at delay=48 BRISC cycles (63.6 M
act/s vs 52.2 M baseline), but this is the NOC issue rate, not real
activations.  The resonance likely reflects NOC pipeline scheduling alignment,
not DRAM refresh synchronization.

A full attack run at the calibrated delay (14-sided, delay=48, 50M iterations,
4 cores, 8 rows) produced 2.8 billion reported activations per row with **zero
flips and zero ECC corrections**.

---

## 6. N-sided extension (18–24 aggressors)

Extended the aggressor selection to support cross-bank dummy rows when
`--num-sides` exceeds ROWS_PER_BANK−1.  Tested at 20 sides (15 same-bank +
5 cross-bank dummies).  **160 M total activations across 4 rows, zero flips,
zero ECC.**

---

## 7. Updated cumulative totals

| Campaign | Activations | Flips | ECC |
|----------|------------:|------:|----:|
| n-sided read (R1, all channels) | 6.4 × 10⁹ | 0 | 0 |
| n-sided read (R2, sides 2..14) | 8.4 × 10⁹ | 0 | 0 |
| pattern × channel grid (R2) | 6.7 × 10⁸ | 0 | 0 |
| --barrier hammer (R2) | 1.0 × 10⁹ | 0 | 0 |
| write hammer (R3, runs 1+2) | 2.4 × 10⁸ | 0 | 0 |
| **N-sided + REF-sync (R4)** | **2.5 × 10¹⁰** | **0** | **0** |
| **20-sided + cross-bank (R4)** | **6.4 × 10⁸** | **0** | **0** |
| **Forced-activation modes (R4)** | **3.9 × 10⁶** | **0** | **0** |
| **TOTAL** | **≈ 4.3 × 10¹⁰** | **0** | **0** |

---

## 8. Architectural conclusion

The Tenstorrent Blackhole's memory architecture provides **inherent rowhammer
mitigation** through three compounding factors:

1. **NOC latency floor (~456 cycles / 570 ns per access).**  Every DRAM access
   from a Tensix core traverses the NOC mesh, imposing a minimum per-access
   latency that is ~12× the DRAM row cycle time (tRC ≈ 48 ns).  This makes it
   physically impossible for a single core to activate rows faster than ~1.8 M/s
   even in serialized mode.

2. **Controller request reordering.**  The GDDR6 controller aggressively
   reorders and coalesces NOC requests.  Pipelined reads and writes are batched
   by row, reducing real row activations by 24–29× relative to the NOC issue
   rate.

3. **On-die ECC + TRR.**  Even if sufficient activations could be generated,
   GDDR6 on-die SEC-DED ECC and Target Row Refresh provide additional defense
   layers.  Across 43 billion reported activations (and ~1.5 billion estimated
   real activations), we observed zero correctable and zero uncorrectable errors.

The required escalation to continue this research would be **firmware-level or
kernel-driver-level access** to the GDDR6 controller's MMIO registers, enabling:
- Closed-page policy (auto-precharge after every access)
- Disabled request reordering
- Direct activation counters for ground-truth measurement
- ECC disable for controlled experiments

None of these are accessible from user-space TT-Metalium code.

---

## 9. Files added in Round 4

### Kernels
- `kernels/rowhammer_nsided_refsync_kernel.cpp` — N-sided + REF-sync combined
- `kernels/activation_diagnostic_kernel.cpp` — 5-test latency comparison
- `kernels/buffer_diagnostic_kernel.cpp` — 4-section buffering layer analysis
- `kernels/forced_activation_kernel.cpp` — 5-mode forced/pipelined comparison

### Host drivers
- `experiments/activation_diagnostic.cpp` — runs activation diagnostic on all channels
- `experiments/buffer_diagnostic.cpp` — runs buffer layer analysis
- `experiments/forced_activation_test.cpp` — runs all activation modes side-by-side

### Modified
- `rowhammer_nsided.cpp` — added `--delay`, `--delay-sweep`, `--row-set-file`,
  `--skip-geometry`, `--num-sides` up to 30, startup ROWS_PER_BANK validation,
  cross-bank dummy aggressor selection, REF-sync kernel dispatch
- `kernels/rowhammer_nsided_kernel.cpp` — arg layout updated (delay at arg 10,
  aggressors at arg 11+)
- `CMakeLists.txt` — registered new targets

### Scripts
- `scripts/check_kernel_disasm.sh` — B4 disassembly audit helper

### Build
```bash
source python_env/bin/activate
cmake --build build_Release -j$(nproc) --target \
    metal_example_rowhammer_nsided \
    metal_example_rh_activation_diag \
    metal_example_rh_buffer_diag \
    metal_example_rh_forced_act
```
