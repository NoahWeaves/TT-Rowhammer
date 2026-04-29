# Blackhole DRAM Geometry

> **⚠️ Partially superseded.** The geometry table below (row size, column/row
> bit splits, page mode) remains valid. The "Hammer rate ~14–17M
> activations/sec" claim is **incorrect** — that's NOC issue rate, not real
> DRAM activation rate. Real per-row ACT rate is ~12 M/s, ~2100× below the
> rowhammer MAC threshold. The "BRISC clock 800 MHz" reference is also
> wrong: wall_clock register increments at ~1.35 GHz on this part. For the
> corrected canonical writeup, see
> [`rowhammer/documentation/FINAL_REPORT.md`](rowhammer/documentation/FINAL_REPORT.md).

**Memory technology:** GDDR6 (8 physical channels, accessed via NOC endpoints)

## Confirmed Parameters

| Parameter | Value | Evidence |
|-----------|-------|----------|
| **Row size** | **8192 bytes (8KB)** | Bit-toggle test: bit 13 is first ROW bit. Conflict matrix: 8KB blocks. |
| **Column select bits** | **[0:12]** (bits 0-12) | Toggling any bit 6-12 produces same-row latency (833 cycles) |
| **Row select bits** | **[13:25+]** | Toggling any bit 13+ produces different-row latency (873-897 cycles) |
| **Rows per channel view** | ~524,288 | 4GB / 8KB = 524,288 rows (per `dram_view_size`) |
| **Address mapping** | Sequential, no XOR | But with 8KB granularity (see interleaving note) |

## Address Mapping

```
Byte address within DRAM bank:
 [31:13]  [12:6]   [5:0]
  ROW     COLUMN   SUB-CL
  |         |        |
  |         |        +-- Byte within 64B cache line (not addressable via NOC)
  |         +----------- Column within row (128 columns × 64B = 8KB)
  +---------------------- Row select (524K rows)
```

## Row Address Examples

| Address Range | Row Number | Notes |
|---------------|------------|-------|
| 0x000000 - 0x001FFF | Row 0 | 0 to 8191 |
| 0x002000 - 0x003FFF | Row 1 | 8192 to 16383 |
| 0x004000 - 0x005FFF | Row 2 | 16384 to 24575 |
| 0x006000 - 0x007FFF | Row 3 | 24576 to 32767 |
| 0x008000 - 0x009FFF | Row 4 | 32768 to 40959 |

## Conflict Matrix (Experiment 4)

The conflict matrix shows a perfect **8KB block-diagonal pattern**:

```
Addresses:     0KB  1KB  2KB  3KB  4KB  5KB  6KB  7KB | 8KB  9KB  ...  15KB
0KB-7KB:       833  833  833  833  833  833  833  833 | 873  873  ...   873
8KB-15KB:      873  873  873  873  873  873  873  873 | 833  833  ...   833
```

- **833 cycles** = same row (row buffer hit on pipelined A-B reads)
- **873 cycles** = different row (row buffer miss, 40-cycle penalty)

This is textbook block-diagonal: all addresses within the same 8KB block share a row.

## Interleaving Note

Experiment 3 tested with a **2KB stride** (which was the wrong row size guess).
With the correct 8KB row size, the "non-conflicts" at rows 1-3 (offsets 0x800, 0x1000, 0x1800)
are simply addresses within the SAME 8KB row as address 0. This is NOT interleaving —
it's expected behavior with 8KB rows. The first conflict appears at 0x2000 (8KB), exactly at
the row boundary.

**Conclusion: Simple sequential addressing. No XOR interleaving detected.**

## Row Buffer Behavior

| Metric | Value | Notes |
|--------|-------|-------|
| Same-row pipelined burst (16 reads) | 770-785 cycles | 48-49 cyc/read |
| Cross-row pipelined burst (16 reads) | 914 cycles | 57 cyc/read |
| Same-row A-B alternating (8 pairs) | 833 cycles | Row buffer hits |
| Cross-row A-B alternating (8 pairs) | 873 cycles | Row buffer misses |
| Single serialized read (with barrier) | 436-444 cycles | NOC overhead dominates |
| **Row buffer hit benefit** | **~40 cycles (50 ns)** | Per access in pipelined mode |
| **Same-addr burst scaling** | **1.74x for 16 reads** | Confirms open-page mode |

## Two Latency Tiers (bits 13-16 vs 17+)

Interesting subtlety: toggling bits 13-16 yields median=873, but toggling bits 17+ yields median=897.
This 24-cycle difference suggests two tiers of "different row":
- **Bits 13-16**: Different row, possibly same bank group or subarray → 873 cycles
- **Bits 17+**: Different row AND different bank group/subarray → 897 cycles

This likely reflects GDDR6 bank-group organization within the channel (GDDR6
exposes 4 bank groups × 4 banks per channel, and cross-bank-group activations
carry a small additional penalty on top of the base tRRD_S/L).

## Implications for Rowhammer

1. **Row size = 8KB**: Each DRAM row spans 8KB of contiguous address space
2. **Adjacent rows are at ±8KB offsets**: Row N is at `N * 0x2000`
3. **For double-sided rowhammer**:
   - Victim row at address `V`
   - Aggressor row 1: `V - 0x2000` (row N-1)
   - Aggressor row 2: `V + 0x2000` (row N+1)
4. **Hammer rate**: With pipelined reads, ~48-57 cycles per activation = ~14-17M activations/sec
   (much faster than the 1.8M/sec from serialized reads)
5. **No interleaving complexity**: Address-to-row mapping is simple division by 8KB

## NOC Port Behavior

Each physical GDDR6 channel on Blackhole is exposed through **three NOC0
sub-ports** (see `tt_metal/soc_descriptors/blackhole_140_arch.yaml`). The three
sub-ports for channel 0, for example, are (0,0), (0,1), and (0,11) — they are
three parallel NOC access paths into the same physical GDDR6 channel, not three
independent banks. Previous testing showed identical latency across all three
sub-ports of channel 0, so the sub-port choice is irrelevant for row-buffer
conflict timing.

Note that "bank" is ambiguous in this stack: tt-metal's allocator uses
`bank_id` for its own per-channel allocator indices, while GDDR6 itself has an
internal bank/bank-group structure (4 bank groups × 4 banks per channel) that
is not directly addressable from the NOC. When this document says "channel"
it means one of the 8 physical GDDR6 channels; when older text says "Bank 0"
it is referring to the same thing (channel 0).

## Test Configuration

- Hardware: Blackhole Tenstorrent (4-chip mesh), GDDR6 DRAM
- BRISC clock: 800MHz (1 cycle = 1.25ns)
- Original single-channel run: DRAM channel 0, NOC endpoint (0,1), 2026-02-26
- Multi-channel verification: all 8 physical GDDR6 channels, 2026-04-10
  (see `validation_bank{0..7}_method{2b,3,5}.txt` and
  `validation_multibank_summary.txt` — 8/8 channels PASS, identical geometry)
- Measurement: On-device wall clock, pipelined NOC reads
