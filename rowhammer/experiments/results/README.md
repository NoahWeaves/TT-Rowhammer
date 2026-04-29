# Saved experimental results

Each `.txt` / `.csv` file in this directory is the captured stdout of a
specific tool invocation. The table below ties each saved result back to its
producing command, the SESSION_VALIDATION claim it supports, and the date
captured.

For the canonical interpretive writeup of these results, see
[`../../documentation/FINAL_REPORT.md`](../../documentation/FINAL_REPORT.md).

| Result file | Producing command | Claim # | Captured |
|---|---|---|---|
| `core_scaling_ch0.txt` | `metal_example_rh_core_scaling` (defaults: ch0, num-sides 8, hammer-iters 50000, sweep 1..128 cores) | claim 1, claim 16 | 2026-04-29 |
| `latency_band_ch0.txt` | `metal_example_rh_latency_band --channel 0 --base-row 30000 --reads-st 4000 --reads-mt 8000` | claim 17 | 2026-04-29 |
| `row_cycle_single_ch0.txt` | `metal_example_rh_row_cycle --channel 0 --sub-port 0 --base-row 30000 --iterations 40000` | claim 14 | 2026-04-29 |
| `row_cycle_mt_ch0.txt` | `metal_example_rh_row_cycle_mt --channel 0 --tiles-per-sp 16 --base-row 30000 --iterations 2000000` | claim 15 | 2026-04-29 |
| `stagger_probe_ch0_n2.txt` | `metal_example_rh_stagger_probe --channel 0 --base-row 30000 --reads 4000 --n-rows 2` | claim 18 | 2026-04-29 |
| `stagger_probe_deep_ch0_n2.txt` | `metal_example_rh_stagger_probe --channel 0 --base-row 30000 --reads 200 --n-rows 2 --deep` | claim 19 | 2026-04-29 |
| `find_trefi_ch0_row1024.csv` | `metal_example_rh_find_trefi` (defaults, ch0, row 1024) | (R3-era, prefigures REF-sync work) | pre-2026-04 |
| `pattern_sweep_ch0.csv` | `metal_example_rh_pattern_sweep` (R1-era multi-pattern × multi-channel hammer) | (R1/R2-era, see `history/RUN_REPORT_R2.md`) | pre-2026-04 |
| `row_set_ch0_anchor1024.csv` | `metal_example_rh_build_row_set` (R1-era row-set construction) | (R1-era, see `history/RUN_REPORT_R1.md`) | pre-2026-04 |
| `row_set_ch0_anchor1024.txt` | `metal_example_rh_build_row_set` (text output of same) | (same) | pre-2026-04 |

## Other saved-data locations in the repo

- `rowhammer/refresh/refresh_ch[0-7]_mode[0-1].txt` and `refresh_summary.txt`
  — outputs of REF-sync delay sweeps, see SESSION_VALIDATION claim 11.
- `rowhammer/validation/validation_bank[0-7]_method[2b|3|5].txt` and
  `validation_multibank_summary.txt` — 8KB-row geometry validation across all
  8 channels, see SESSION_VALIDATION geometry claims and the
  `verify_geometry` / `validation_probe` reproducers.
- `rowhammer/validation/validation_8kb_boundaries.txt`,
  `validation_column_independence.txt`, and `read_inspect.txt` — original
  Feb-2026-era geometry validation outputs (still valid).

## Reproducing all of these in one go

```bash
cd ~/tt-metal && source python_env/bin/activate
cmake --build build_Release -j$(nproc) --target \
    metal_example_rh_core_scaling \
    metal_example_rh_row_cycle \
    metal_example_rh_row_cycle_mt \
    metal_example_rh_latency_band \
    metal_example_rh_stagger_probe

# tRC bound (claim 16) — ~30 sec
./build_Release/programming_examples/metal_example_rh_core_scaling \
    > rowhammer/experiments/results/core_scaling_ch0.txt

# Row-buffer cache depth, single-thread (claim 14) — ~10 sec
./build_Release/programming_examples/metal_example_rh_row_cycle \
    --channel 0 --sub-port 0 --base-row 30000 --iterations 40000 \
    > rowhammer/experiments/results/row_cycle_single_ch0.txt

# Multi-thread row-cycle (claim 15) — ~5 sec
./build_Release/programming_examples/metal_example_rh_row_cycle_mt \
    --channel 0 --tiles-per-sp 16 --base-row 30000 --iterations 2000000 \
    > rowhammer/experiments/results/row_cycle_mt_ch0.txt

# Latency-band probe (claim 17) — ~20 sec
./build_Release/programming_examples/metal_example_rh_latency_band \
    --channel 0 --base-row 30000 --reads-st 4000 --reads-mt 8000 \
    > rowhammer/experiments/results/latency_band_ch0.txt

# Stagger-invariance (claim 18) — ~30 sec
./build_Release/programming_examples/metal_example_rh_stagger_probe \
    --channel 0 --base-row 30000 --reads 4000 --n-rows 2 \
    > rowhammer/experiments/results/stagger_probe_ch0_n2.txt

# Deep-stagger (claim 19) — ~5 min (high-stagger runs are long by design)
./build_Release/programming_examples/metal_example_rh_stagger_probe \
    --channel 0 --base-row 30000 --reads 200 --n-rows 2 --deep \
    > rowhammer/experiments/results/stagger_probe_deep_ch0_n2.txt
```
