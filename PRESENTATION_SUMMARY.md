# Blackhole DRAM Geometry: Validated Findings

> **⚠️ Partially superseded.** The geometry findings below (8 KB row size,
> address mapping, per-channel layout) remain valid. The activation-rate and
> rowhammer-feasibility claims (e.g., "14–17M activations/sec exceeding the
> threshold by 25–100×") are **incorrect** — those rates are NOC issue rates,
> not real DRAM activations. Real per-row ACT rate is ~12 M/s, ~2100× below
> threshold. Likewise the "BRISC clock 800 MHz" assumption is wrong — the
> wall_clock register increments at ~1.35 GHz on this part. For the
> corrected canonical writeup, see
> [`rowhammer/documentation/FINAL_REPORT.md`](rowhammer/documentation/FINAL_REPORT.md).
> For per-claim reproducers, see
> [`rowhammer/documentation/SESSION_VALIDATION.md`](rowhammer/documentation/SESSION_VALIDATION.md).

**Date:** 2026-02-26 (single-channel), extended 2026-04-10 (all 8 channels)
**Hardware:** Tenstorrent Blackhole — **GDDR6**, 8 physical channels (~4 GB each)
**Original target:** DRAM channel 0, NOC endpoint (0,1)
**Multi-channel verification:** all 8 GDDR6 channels swept via NOC endpoints
  (0,1), (0,10), (0,4), (0,7), (9,1), (9,10), (9,4), (9,7) — see
  `validation_multibank_summary.txt`
**BRISC Clock:** 800MHz (1 cycle = 1.25ns)

## Key Finding: 8KB Row Size (Validated)

### Evidence Summary

| # | Validation Method | Tests | Result | Status |
|---|-------------------|-------|--------|--------|
| 1 | Explicit Boundary Test (21 address pairs) | 21/21 | Every pair classified correctly | PASS |
| 2 | 8KB Conflict Matrix (8x8 at 8KB step) | 64/64 | Perfect diagonal: 833 cyc, off-diag: 873 cyc | PASS |
| 2b | Sub-Row Matrix (16x16 at 1KB step) | 256/256 | Two clean 8x8 blocks on diagonal | PASS |
| 3 | Stride Sweep (6KB-10KB in 128B steps) | 33/33 | Sharp transition at **exactly** 8192 bytes | PASS |
| 4 | Multiple Base Addresses (0 to 512MB) | 7/7 | Consistent 40-cycle delta everywhere | PASS |
| 5 | Bit-Toggle Confirmation (bits 6-25) | 20/20 | bits 6-12 = COL, bits 13+ = ROW | PASS |
| | **TOTAL** | **401/401** | **100% pass rate** | **PASS** |

### Address Mapping

```
Byte Address: [31 ........... 13][12 ...... 6][5 ... 0]
              └── ROW SELECT ──┘ └─ COLUMN ─┘ └ BYTE ┘
              (524K rows)        (128 cols)    (64B/txn)
```

- **Row select:** Bits [13:31] --> Row number = `Address >> 13`
- **Column select:** Bits [6:12] --> 128 columns per row
- **Byte select:** Bits [0:5] --> 64 bytes per NOC transaction

### Row Examples

| Row | Address Range | Hex Range |
|-----|---------------|-----------|
| 0 | 0 - 8,191 | 0x000000 - 0x001FFF |
| 1 | 8,192 - 16,383 | 0x002000 - 0x003FFF |
| 2 | 16,384 - 24,575 | 0x004000 - 0x005FFF |
| N | N x 8192 - (N+1) x 8192 - 1 | N x 0x2000 |

### Performance Characteristics

| Metric | Value | Context |
|--------|-------|---------|
| Same-row A-B alternating (8 pairs) | 833 cycles | Row buffer hits |
| Cross-row A-B alternating (8 pairs) | 873 cycles | Row buffer misses |
| Row buffer benefit | **40 cycles (50 ns)** | Per pipelined access |
| Same-row burst (16 reads) | 770 cycles total | 48 cyc/read |
| Cross-row burst (16 reads, 64KB stride) | 914 cycles total | 57 cyc/read |
| Serialized single read | 436 cycles | NOC overhead dominates |
| Same-addr burst=16 / burst=1 ratio | 1.74x | Confirms open-page mode |

### Two Latency Tiers in ROW Bits

| Bits | Median | Interpretation |
|------|--------|----------------|
| 13-16 (8KB-64KB) | 873 cycles | Different row, same bank group |
| 17-25 (128KB-32MB) | 897 cycles | Different row, different bank group |

The 24-cycle difference likely reflects GDDR6 bank-group structure within the
channel (4 bank groups × 4 banks per channel in standard GDDR6 organization).
Toggling bits 13-16 stays within one bank group; toggling bit 17+ crosses into
a different bank group and incurs the extra tRRD_S → tRRD_L transition cost.

### Address Interleaving

**None detected.** Sequential addresses map to sequential rows. No XOR hashing.

### Rowhammer Implications

| Parameter | Value |
|-----------|-------|
| Adjacent rows | +/- 0x2000 (8KB) offset |
| Double-sided pattern | Victim at V, aggressors at V-0x2000 and V+0x2000 |
| Pipelined activation rate | ~14-17M activations/sec |
| Acts per 32ms refresh window | 450K-550K |
| Typical TRH threshold | 5K-20K (likely exceeded by 25-100x) |

## Methodology

All measurements used:
- On-device BRISC wall clock (800MHz, 1.25ns resolution)
- Pipelined NOC reads (no barriers inside measurement loop)
- Alternating A-B read patterns (8 pairs per test)
- Median of 32-64 samples per measurement point
- Row buffer flush between measurements (read from addr + 128MB)

## Sources

1. **Empirical measurement** (this work) -- 401 tests, 100% pass rate on channel 0;
   multi-channel sweep confirms identical geometry on all 8 GDDR6 channels
2. **JEDEC JESD250** (GDDR6 standard) -- 8KB row size (1 KB × 8 bit-prefetch)
   is typical for modern GDDR6 devices
3. No GDDR6-specific documentation found in tt-metal codebase. Row size was
   determined purely empirically.

## Validation Status

- [x] All 401 single-channel validation tests passed
- [x] 5 independent methods agree
- [x] Multi-channel sweep: all 8 physical GDDR6 channels show identical
      8KB / bit-13 geometry (see `validation_bank{0..7}_*.txt`)
- [x] Consistent with GDDR6 JEDEC geometry
- [x] Validated across 7 different DRAM base addresses (0 to 512MB)
- [x] Reproducible on all runs
- [x] Ready for rowhammer experimentation
