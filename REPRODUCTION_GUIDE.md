# How to Reproduce These Findings

> **⚠️ Partially superseded.** This Apr-10-era guide reproduces the geometry
> validation but predates the rowhammer attack campaign and the 4-method tRC
> ceiling validation. For the comprehensive reproducer index covering all 19
> claims (geometry, attacks, ceilings, coalescing decomposition), see
> [`rowhammer/documentation/SESSION_VALIDATION.md`](rowhammer/documentation/SESSION_VALIDATION.md).
> For the canonical narrative, see
> [`rowhammer/documentation/FINAL_REPORT.md`](rowhammer/documentation/FINAL_REPORT.md).

## Prerequisites

- Tenstorrent Blackhole hardware with tt-metal SDK installed
- BRISC kernel compilation environment (cmake + clang)
- Python 3.8+ with numpy, matplotlib

## Build

```bash
cd ~/tt-metal
cmake --build build --target metal_example_validation_test -j$(nproc)
cmake --build build --target metal_example_row_mapping_test -j$(nproc)
cmake --build build --target metal_example_page_mode_test -j$(nproc)
```

## Step 1: Run Full Validation Suite (recommended first)

```bash
./build/programming_examples/metal_example_validation_test
```

Expected output: `ALL TESTS PASSED` (401/401)

This runs 5 independent methods:
- Method 1: 21 explicit boundary tests
- Method 2: 8x8 conflict matrix at 8KB granularity
- Method 2b: 16x16 conflict matrix at 1KB granularity
- Method 3: Fine-grained stride sweep around 8KB
- Method 4: Same test at 7 different DRAM base addresses
- Method 5: Individual address bit toggle classification

Total runtime: ~1-2 seconds.

## Step 2: Run Row Mapping Discovery (optional)

```bash
./build/programming_examples/metal_example_row_mapping_test
```

This runs the original discovery experiments that identified the 8KB row size.

## Step 3: Run Page Mode Test (optional)

```bash
./build/programming_examples/metal_example_page_mode_test
```

This confirms open-page mode behavior via burst read scaling.

## Step 4: Generate Plots

```bash
cd rowhammer
python3 generate_presentation_plots.py
```

Output files:
- `validation_8kb_matrix.png` - conflict matrix heatmap
- `validation_stride_sweep_8kb.png` - stride sweep detail
- `validation_bit_mapping.png` - address bit classification
- `validation_performance.png` - performance comparison
- `validation_dashboard.png` - test summary dashboard

## Step 5: Verify Against Different Hardware

Blackhole uses GDDR6 (8 physical channels accessed via NOC endpoints). If
testing on a different GDDR6 SKU or a different DRAM technology entirely
(GDDR6X, HBM, LPDDR, etc.) the row size may differ (2KB, 4KB, 16KB are also
possible). Run the page mode test first to confirm open-page behavior, then
the row mapping test to find the actual row size.

## Code Locations

| File | Purpose |
|------|---------|
| `tt_metal/programming_examples/dram_latency/kernels/validation_probe.cpp` | Validation BRISC kernel |
| `tt_metal/programming_examples/dram_latency/validation_test.cpp` | Validation host program |
| `tt_metal/programming_examples/dram_latency/kernels/row_mapping_probe.cpp` | Discovery BRISC kernel |
| `tt_metal/programming_examples/dram_latency/row_mapping_test.cpp` | Discovery host program |
| `tt_metal/programming_examples/dram_latency/kernels/page_mode_probe.cpp` | Page mode BRISC kernel |
| `tt_metal/programming_examples/dram_latency/page_mode_test.cpp` | Page mode host program |
| `rowhammer/generate_presentation_plots.py` | Plot generation |
| `rowhammer/PRESENTATION_SUMMARY.md` | Executive summary |
| `rowhammer/summary_dram_geometry.md` | Technical reference |
