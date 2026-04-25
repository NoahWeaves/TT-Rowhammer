# Replication Steps

How to reproduce the work in this repository on a fresh machine. This guide
assumes you already have:

- A Tenstorrent **Blackhole** card (8-channel GDDR6, 4 GB/channel).
- [`tt-metal`](https://github.com/tenstorrent/tt-metal) cloned and able to
  build on the host (i.e. you can already build and run any stock
  `programming_examples/`).
- Python 3.8+ with `pyluwen`, `numpy`, `matplotlib` installed
  (`pyluwen` is required to read GDDR6 ECC telemetry counters).
- This repo (`TT-Rowhammer`) cloned somewhere on disk; we'll refer to it as
  `$TT_ROWHAMMER`.

The repo is split into two halves:

| Folder | Purpose | Where it has to live to build |
|--------|---------|-------------------------------|
| `TT-Rowhammer/` (top level) | DRAM characterization data, plots, validation scripts. Drives the `dram_latency` programming example in tt-metal. | Standalone; only Python scripts run from here. |
| `TT-Rowhammer/rowhammer/`   | Rowhammer host drivers + BRISC kernels. Built as a tt-metal programming example. | Symlinked or copied into `tt-metal/tt_metal/programming_examples/rowhammer/`. |

End-to-end the replication has three phases, mirroring how the project was
originally executed:

1. **Phase A — DRAM characterization** (validate row size / address layout).
2. **Phase B — Geometry verification** (confirm the assumptions the hammer
   code relies on, on *your* board).
3. **Phase C — Run the hammer experiments** and collect ECC telemetry.

---

## 0. One-time setup: place the rowhammer sources inside tt-metal

The build rules in [`rowhammer/CMakeLists.txt`](../CMakeLists.txt) expect the
folder to be a tt-metal programming example. The cleanest way to hook it in
without copying:

```bash
# Pick a place outside tt-metal to keep this repo
export TT_ROWHAMMER=$HOME/TT-Rowhammer
export TT_METAL=$HOME/tt-metal

# Symlink the rowhammer/ folder into tt-metal's programming_examples
ln -s "$TT_ROWHAMMER/rowhammer" \
      "$TT_METAL/tt_metal/programming_examples/rowhammer"
```

Then register the new subdirectory with the rest of the programming
examples. Edit `tt-metal/tt_metal/programming_examples/CMakeLists.txt` and
add a line near the other `add_subdirectory(...)` entries:

```cmake
add_subdirectory(rowhammer)
```

If you also want the standalone DRAM characterization binaries
(`metal_example_validation_test`, `metal_example_row_mapping_test`,
`metal_example_page_mode_test`) referenced by
[`REPRODUCTION_GUIDE.md`](../../REPRODUCTION_GUIDE.md), those live in
`tt-metal/tt_metal/programming_examples/dram_latency/` and were authored as
part of the original work but are not redistributed in this repo. If you do
not have them, skip Phase A and rely on the captured artefacts in
`TT-Rowhammer/` (`conflict_matrix.csv`, `validation_*.txt`, etc.) as the
ground-truth reference values for Phase B.

---

## 1. Build

From `tt-metal`, use whatever build preset / `CMAKE_BUILD_TYPE` your tree is
configured for. The examples below assume a `build_Release/` tree:

```bash
cd "$TT_METAL"
cmake --build build_Release -j"$(nproc)" --target \
    metal_example_rowhammer \
    metal_example_rowhammer_nsided \
    metal_example_rh_verify_geometry \
    metal_example_rh_test_addr_decoder \
    metal_example_rh_find_trefi \
    metal_example_rh_pattern_sweep \
    metal_example_rh_build_row_set \
    metal_example_rh_write_hammer \
    metal_example_rh_write_hammer_multi
```

All host binaries land under
`build_Release/programming_examples/rowhammer/` (or
`build_Release/programming_examples/` for the n-sided / write-hammer
targets — exact path depends on your tt-metal version; check both).

> **Note:** the BRISC kernels under `rowhammer/kernels/` are *not* built
> here. They are JIT-compiled by tt-metal when the host program launches
> them, using the path passed via `CreateKernel(..., "kernels/...")`. As long
> as the symlink in §0 is in place, runtime kernel discovery just works.

---

## 2. Phase A — DRAM characterization (optional, skip if you trust the captured data)

If you have the `dram_latency` programming example available in your
tt-metal tree:

```bash
cd "$TT_METAL"
./build_Release/programming_examples/metal_example_validation_test
# Expected: ALL TESTS PASSED (401/401)

./build_Release/programming_examples/metal_example_row_mapping_test
./build_Release/programming_examples/metal_example_page_mode_test

cd "$TT_ROWHAMMER"
python3 generate_presentation_plots.py
python3 verify_data_consistency.py
```

Cross-check your output against the captured tables in
[`TT-Rowhammer/README.md`](../../README.md) and the raw files
(`conflict_matrix.csv`, `address_bit_mapping.txt`,
`row_size_determination.txt`, `row_verification.txt`,
`validation_*.txt`). The headline numbers to confirm on your board:

- 8 KB row size (`addr >> 13`)
- Row-buffer hit ≈ **833 cyc**, miss ≈ **873 cyc**, cross-bank-group ≈ **897 cyc**
- All 8 GDDR6 channels show the same geometry

If those numbers do not match, *stop here* — the address-construction
helpers in `rowhammer.cpp` / `rowhammer_nsided.cpp` and
`experiments/dram_addr_decoder.hpp` will be wrong for your part.

---

## 3. Phase B — Geometry verification (mandatory, must pass on your board)

These are the gating checks before running the hammer. They re-derive (on
your hardware) the geometry constants the hammer kernels use. See the
existing [`verification_report.md`](verification_report.md) for the format.

### B1 + B2 — automated sweeps and address-decoder unit test

```bash
cd "$TT_METAL"

# 8 KB conflict matrix per channel + cross-bank-group jump search.
# Rewrites rowhammer/documentation/verification_report.md in place.
./build_Release/programming_examples/metal_example_rh_verify_geometry

# Pure-host unit test for the address decoder; PASS = all expected pairs
# decoded correctly + invariants hold.
./build_Release/programming_examples/metal_example_rh_test_addr_decoder
```

### B3 — ECC baseline (manual, but cheap)

This run intentionally hammers a small range. The pass condition is that
the GDDR6 correctable-error counters move when hammering and that no
readback flips appear (i.e. ECC is silently masking single-bit flips, as
expected on Blackhole):

```bash
./build_Release/programming_examples/rowhammer/metal_example_rowhammer \
    --channel 0 --start-row 64 --num-rows 64 --iterations 500000
```

Note: on the original hardware this campaign produced **0 ECC events and 0
flips** across ~1.7 × 10¹⁰ activations — see
[`RUN_REPORT_R3.md`](RUN_REPORT_R3.md). Do not interpret a clean run as a
failure of the *test*; it is the documented behaviour of the platform.

### B4 — Compiler-fold sanity (manual)

After at least one hammer run, locate the JIT-compiled BRISC ELF for
`rowhammer_kernel.cpp` (under `generated/<run_id>/.../trisc0/` or
similar — `tt-metal`'s watcher logs in `generated/watcher/kernel_elf_paths.txt`
list exact paths) and confirm the inner hammer loop contains exactly two
NOC issue points:

```bash
riscv-tt-elf-objdump -d <kernel.elf> | grep -A2 noc_fast_read | less
```

Phase C is gated on B1 + B2 + B5 passing. Record B3 / B4 outcomes in
`verification_report.md` before continuing.

---

## 4. Phase C — Run the hammer experiments

The experiments in this repo all share the same result schema:
`status, num_flips, total_bit_flips, cycles_lo/hi, activations`, plus up to
32 detailed mismatch records `(cl, word, expected, actual)`. Wrap any of
them in `experiments/ecc_hammer.py` to also capture the GDDR6 ECC counter
deltas (this is the only reliable way to detect single-bit flips that
on-die ECC silently corrects):

```bash
cd "$TT_METAL"

# Double-sided pipelined hammer (V±0x2000), single core, all 8 channels.
python3 tt_metal/programming_examples/rowhammer/experiments/ecc_hammer.py \
    ./build_Release/programming_examples/rowhammer/metal_example_rowhammer -- \
    --all-channels --num-rows 64 --hammer-iterations 500000

# N-sided multi-core hammer (TRR overflow attempt).
python3 tt_metal/programming_examples/rowhammer/experiments/ecc_hammer.py \
    ./build_Release/programming_examples/rowhammer/metal_example_rowhammer_nsided -- \
    --channel 0 --num-rows 16 --iterations 5000000 \
    --num-sides 14 --num-cores 4

# Write-based hammer (cannot be served from row buffer).
python3 tt_metal/programming_examples/rowhammer/experiments/ecc_hammer.py \
    ./build_Release/programming_examples/metal_example_rh_write_hammer -- \
    --channel 0 --num-rows 4 --iterations 5000000 \
    --num-aggressors 30 --stride 1
```

Other experiments under `rowhammer/experiments/` (`find_trefi`,
`pattern_sweep`, `build_row_set`, `write_hammer_multi`) follow the same
pattern and write their structured outputs into
`tt_metal/programming_examples/rowhammer/experiments/results/`. They expect
to be invoked from the tt-metal repo root so that the relative output path
resolves correctly:

```bash
cd "$TT_METAL"
./build_Release/programming_examples/metal_example_rh_find_trefi
./build_Release/programming_examples/metal_example_rh_pattern_sweep
./build_Release/programming_examples/metal_example_rh_build_row_set
./build_Release/programming_examples/metal_example_rh_write_hammer_multi --help
```

---

## 5. Interpreting results

Everything you need is in [`RUN_REPORT.md`](RUN_REPORT.md),
[`RUN_REPORT_R2.md`](RUN_REPORT_R2.md), and
[`RUN_REPORT_R3.md`](RUN_REPORT_R3.md). The condensed expectation:

- **Visible bit flips on readback:** 0. Per-row activation cost is not
  visible from user space; on-die ECC + TRR/RFM mitigations are active.
- **GDDR6 `*_corr_errs` deltas:** 0 across all campaigns run so far
  (~1.7 × 10¹⁰ activations).
- **Achieved hammer rate:** ~14–17 M acts/s pipelined-read,
  ~26 M acts/s for the write-based variant. A rate that does not vary with
  address stride (within four significant figures) is the documented
  symptom that NoC injection / write-combining — not bank activation — is
  the bottleneck.

If you observe anything different (especially non-zero ECC deltas or any
readback flip), capture the full stdout, the `experiments/results/` JSON,
and the `generated/watcher/` logs, and document the run in a new
`RUN_REPORT_*.md` alongside the existing ones.

---

## 6. Cleanup

```bash
# Remove the symlink (does not touch this repo)
rm "$TT_METAL/tt_metal/programming_examples/rowhammer"
# Revert the add_subdirectory(rowhammer) edit in tt-metal's
# programming_examples/CMakeLists.txt.
```
