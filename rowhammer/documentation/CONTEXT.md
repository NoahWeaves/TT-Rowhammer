# Rowhammer on Tenstorrent — Context Document

This document is the single source of truth for what we currently know, what we
have built, and what external work we are leaning on. Read this before making
any change in the working folder
[`tt-metal/tt_metal/programming_examples/rowhammer/`](../).

---

## 1. Project goal

Reproduce a classic Rowhammer attack on a **Tenstorrent Blackhole** card with
**GDDR6** memory: rapidly activate aggressor DRAM rows from user-level code on
the Tensix mesh and observe persistent bit flips in adjacent victim rows. The
end goal is at least one confirmed flip (readback mismatch or `gddr_uncorr_errs`
delta), as a foundation for later work on weight-tampering exploits in the
GPUHammer style.

---

## 2. DRAM characterization (TT-Rowhammer repo)

All of the following was measured on-device with a BRISC timing kernel
(800 MHz wall clock, 1.25 ns/cycle) using pipelined NOC reads, and validated
across 401 independent tests with a 100 % pass rate. Sources: [TT-Rowhammer/README.md](../../../../../TT-Rowhammer/README.md),
[TT-Rowhammer/summary_dram_geometry.md](../../../../../TT-Rowhammer/summary_dram_geometry.md),
[TT-Rowhammer/conflict_matrix.csv](../../../../../TT-Rowhammer/conflict_matrix.csv),
[TT-Rowhammer/address_bit_mapping.txt](../../../../../TT-Rowhammer/address_bit_mapping.txt),
[TT-Rowhammer/row_size_determination.txt](../../../../../TT-Rowhammer/row_size_determination.txt),
[TT-Rowhammer/row_verification.txt](../../../../../TT-Rowhammer/row_verification.txt).

| Property                     | Value                                              | Evidence |
|------------------------------|----------------------------------------------------|----------|
| Row size                     | **8192 B (8 KB)**                                  | bit-13 toggle latency split + stride sweep transition exactly at 8192 |
| Address mapping              | **Sequential, no XOR interleaving**                | per-bit toggle test on bits 6–25 |
| Page mode                    | **Open page**                                      | row-buffer hit/miss penalty observable |
| Channels                     | **8 GDDR6** (NOC endpoints `(0,1)(0,10)(0,4)(0,7)(9,1)(9,10)(9,4)(9,7)`) | multi-channel sweep, identical geometry |
| Same-bank rows (assumed)     | **16 rows × 8 KB = 128 KB per bank**               | 873→897 cyc cross-bank-group jump (inferred, **not directly measured**) |
| Row-buffer hit latency       | **833 cyc** (~1.04 µs)                             | Method 2 (8 KB conflict matrix) |
| Row-buffer miss latency      | **873 cyc**                                        | bits 13–16 toggle |
| Cross-bank-group latency     | **897 cyc**                                        | bits 17+ toggle |
| Pipelined activation rate    | **~14–17 M acts/s** (48–57 cyc/activation)         | hammer kernel timing |
| Serialized activation rate   | **~1.8 M acts/s** (~436 cyc/activation)            | barrier-after-each-read variant |

### Address layout

```
Byte address bits:  [31:13]   [12:6]   [5:0]
                     ROW       COLUMN   BYTE
```

Helpful identities (these are the address-construction primitives we use):

- `row_id(addr)         = addr >> 13`
- `aggressor_lo(victim) = victim - 0x2000`
- `aggressor_hi(victim) = victim + 0x2000`
- `same_row(a, b)       = (a >> 13) == (b >> 13)`

---

## 3. Hammer infrastructure already in place

Everything lives under
[`tt-metal/tt_metal/programming_examples/rowhammer/`](../).

### 3.1 Host programs

- [`rowhammer.cpp`](../rowhammer.cpp) — double-sided driver. Allocates an L1
  scratch + result buffer per worker core, sweeps a configurable range of
  victim rows on a configurable channel, launches one program per row, parses
  back a fixed-schema result buffer (`status`, `num_flips`, `total_bit_flips`,
  `cycles_lo/hi`, `activations`, then up to 32 `[cl, word, expected, actual]`
  records).
- [`rowhammer_nsided.cpp`](../rowhammer_nsided.cpp) — n-sided + multi-core
  driver. Same result schema. Implements `select_same_bank_aggressors()` which
  currently relies on the **assumed** `ROWS_PER_BANK = 16`.

CLI flags include `--channel`, `--start-row`, `--num-rows`,
`--hammer-iterations`, `--pattern`, `--barrier`, `--all-channels`, plus
`--num-sides`, `--num-cores` for the n-sided variant.

### 3.2 Kernels

- [`kernels/rowhammer_kernel.cpp`](../kernels/rowhammer_kernel.cpp) — three
  phases (pattern setup → tight pipelined hammer loop → readback compare).
  Pipelined loop has **no barrier inside** the loop body, which is the single
  most important throughput optimization (see TT-Rowhammer notes). Activations
  per row = `hammer_iters * 2`.
- [`kernels/rowhammer_nsided_kernel.cpp`](../kernels/rowhammer_nsided_kernel.cpp)
  — round-robin sweep over up to 15 same-bank aggressors, with a `core_id`
  branch so only core 0 writes patterns and verifies; all cores hammer.
  Activations per core = `hammer_iters * num_aggressors`.

### 3.3 Geometry constants used by the kernels

```cpp
constexpr uint32_t ROW_SIZE           = 8192;   // 8 KB
constexpr uint32_t CACHELINE          = 64;     // bytes per NOC txn
constexpr uint32_t CACHELINES_PER_ROW = 128;
constexpr uint32_t ROWS_PER_BANK      = 16;     // INFERRED, see §6
constexpr double   NS_PER_CYCLE       = 1.25;   // 800 MHz BRISC
```

Result schema (head):

```cpp
uint32_t status;
uint32_t num_flips;        // cache lines with >=1 bit flip
uint32_t total_bit_flips;  // sum of individual flips
uint32_t cycles_lo, cycles_hi;
uint32_t activations;
// followed by up to 32 records of [cl_idx, word_offset, expected, actual]
```

Default tuning (current code):

| Parameter           | `rowhammer.cpp` | `rowhammer_nsided.cpp` |
|---------------------|-----------------|------------------------|
| `start_row`         | 32              | 1000                   |
| `num_rows_to_test`  | 64              | 16                     |
| `hammer_iterations` | 500 000         | 5 000 000              |
| `data_pattern`      | `0x55555555`    | `0x55555555`           |
| `num_sides`         | —               | 14 (cap 15)            |
| `num_cores`         | —               | 4                      |

---

## 4. ECC: how we know it exists

Tenstorrent GDDR6 channels expose per-channel-pair ECC counters via
**pyluwen** telemetry. Both host programs shell out:

```python
from pyluwen import PciChip
t = PciChip(pci_interface=0).get_telemetry()
# gddr01_corr_errs, gddr23_corr_errs, gddr45_corr_errs, gddr67_corr_errs,
# gddr_uncorr_errs
```

The drivers read counters before the run, every 4–8 victim rows, and at the
end, then print:

```
*** ECC IS MASKING ROWHAMMER BIT FLIPS ***
The attack IS causing physical bit flips, but on-die ECC
is correcting them before they reach the NOC readback.
```

whenever `corr_errs` increments while readback shows zero flips. That
juxtaposition (`+corr_errs` ∧ `0 readback flips`) is our current evidence that:

1. Single-bit Rowhammer flips **are physically occurring** on the chip.
2. **On-die SEC ECC** is correcting them before the data reaches the NOC.
3. Defeating ECC requires either a firmware toggle (none documented) or
   inducing **multi-bit flips per ECC word** (ECCploit-style).

There is currently **no test** that explicitly disables ECC or quantifies its
correction strength, only the indirect counter-delta evidence above.

---

## 5. External reference work

### 5.1 GPUHammer (USENIX Security 2025)

Repo: <https://github.com/sith-lab/gpuhammer> · Site: <https://gpuhammer.com/>
· Paper: SEC25_GPUHammer.pdf.

First demonstration of Rowhammer on GPU memory (NVIDIA A6000 / GDDR6, with
ECC disabled). Three steps directly applicable to our work:

1. **Reverse-engineer GPU DRAM mappings** with timing side channels (DRAMA
   technique, filtered for NUMA effects). They generate per-bank "row sets"
   of addresses that conflict in the row buffer.
2. **Maximize hammering intensity** by exploiting SIMT parallelism — multi-warp
   multi-thread reads to keep the memory controller saturated. Our analogue is
   multi-core multi-NOC-sub-port hammering.
3. **Synchronize to refresh** with implicit per-warp delays so that aggressor
   activations land in between DRAM auto-refresh windows, defeating in-DRAM
   TRR and similar mitigations. They use 8/12/16/20/**24-sided** patterns.

Result on A6000: ≥8 distinct single-bit flips, TRH ≈ 12 K activations. With
one flipped FP16 exponent MSB, ImageNet model accuracy drops 80 % → 0.1 %.

### 5.2 Other prior art we lean on

- Kim et al., **Rowhammer**, ISCA 2014 — original effect, pattern guidance.
- Frigo et al., **TRRespass**, S&P 2020 — n-sided patterns to overflow TRR.
- de Ridder et al., **SMASH**, USENIX 2021 — refresh-synchronized hammering
  from JavaScript.
- Jattke et al., **BlackSmith**, S&P 2022 — non-uniform synchronized patterns.
- Cojocar et al., **ECCploit**, USENIX 2019 — multi-bit flips for ECC bypass.
- Pessl et al., **DRAMA**, USENIX 2016 — bank reverse-engineering by latency.

---

## 6. Known unknowns / risks

These are pulled from
[TT-Rowhammer/Summary_Main_Personal.md](../../../../../TT-Rowhammer/Summary_Main_Personal.md)
and code review. They drive the verification + new-attack work in
[`PLAN.md`](PLAN.md).

| Item                                        | Risk     | Notes |
|---------------------------------------------|----------|-------|
| `ROWS_PER_BANK = 16` is **inferred**, not measured | High     | Drives `select_same_bank_aggressors()` in n-sided code. |
| No REF synchronization                      | High     | All published GPU/CPU successes use it once basic hammer fails. |
| Aggressor data pattern is fixed (`~victim`) | Medium   | Different patterns flip different cells. |
| ECC strength / disable path unknown         | Critical | If on-die SEC-DED is unbypassable, only multi-bit attempts can succeed. |
| Compiler-fold of hammer loop unverified     | Medium   | BRISC compiler not previously audited for this loop. |
| Temperature uncontrolled                    | High     | Rowhammer is strongly temperature-dependent. |
| Single NOC sub-port used                    | Medium   | Each channel has 3 sub-ports (`blackhole_140_arch.yaml`); we use one. |
| No same-bank set verified per-channel       | Medium   | We assume per-channel geometry is identical; only confirmed for layout, not vulnerability. |

---

## 7. File index

### Working folder (`tt-metal/tt_metal/programming_examples/rowhammer/`)

- [`rowhammer.cpp`](../rowhammer.cpp) — double-sided host driver.
- [`rowhammer_nsided.cpp`](../rowhammer_nsided.cpp) — n-sided + multi-core host
  driver.
- [`kernels/rowhammer_kernel.cpp`](../kernels/rowhammer_kernel.cpp) — BRISC
  hammer kernel (double-sided).
- [`kernels/rowhammer_nsided_kernel.cpp`](../kernels/rowhammer_nsided_kernel.cpp)
  — BRISC hammer kernel (n-sided / multi-core).
- [`CMakeLists.txt`](../CMakeLists.txt) — build rules.
- [`documentation/PLAN.md`](PLAN.md) — actionable roadmap (sibling document).

### TT-Rowhammer reference data (`/TT-Rowhammer/`)

- [README.md](../../../../../TT-Rowhammer/README.md) · [REPRODUCTION_GUIDE.md](../../../../../TT-Rowhammer/REPRODUCTION_GUIDE.md) · [Summary_Main_Personal.md](../../../../../TT-Rowhammer/Summary_Main_Personal.md) · [PRESENTATION_SUMMARY.md](../../../../../TT-Rowhammer/PRESENTATION_SUMMARY.md)
- [summary_dram_geometry.md](../../../../../TT-Rowhammer/summary_dram_geometry.md)
- [conflict_matrix.csv](../../../../../TT-Rowhammer/conflict_matrix.csv) — full latency matrix.
- [address_bit_mapping.txt](../../../../../TT-Rowhammer/address_bit_mapping.txt) — per-bit toggle latency (bits 6–25).
- [row_size_determination.txt](../../../../../TT-Rowhammer/row_size_determination.txt) — stride sweep.
- [row_verification.txt](../../../../../TT-Rowhammer/row_verification.txt) — explicit boundary pairs.
- [verify_data_consistency.py](../../../../../TT-Rowhammer/verify_data_consistency.py) — cross-validation script (model for our verification harness).

### Hardware descriptors

- `tt-metal/tt_metal/soc_descriptors/blackhole_140_arch.yaml` — 8 GDDR6
  channels × 3 NOC sub-ports each.
