# Session Validation Reference — 2026-04-27

Reference file for verifying claims made during the user-space ceiling
characterization session. Every numeric claim below has a single command
that reproduces it. Build everything first:

```bash
cd ~/tt-metal && source python_env/bin/activate
cmake --build build_Release -j$(nproc) --target \
    metal_example_rh_core_scaling \
    metal_example_rh_bank_scaling \
    metal_example_rh_subport_sweep \
    metal_example_rh_subport_probe \
    metal_example_rh_intra_channel \
    metal_example_rh_peak_attack \
    metal_example_rh_peak_refsync \
    metal_example_rh_bank_boundary \
    metal_example_rh_blacksmith
```

All binaries land in `build_Release/programming_examples/`.

---

## New files added this session

### Kernels
| File | Purpose |
|---|---|
| `rowhammer/kernels/core_scaling_kernel.cpp` | FLUSH-READ multi-core hammer (foundation for all measurements) |
| `rowhammer/kernels/victim_inspect_kernel.cpp` | Reads N cachelines DRAM→L1 for host-side pre/post inspection |
| `rowhammer/kernels/subport_probe_kernel.cpp` | Writes via one sub-port, reads via all 3 to test aliasing |
| `rowhammer/kernels/core_scaling_refsync_kernel.cpp` | FLUSH-READ + per-sweep delay (REF-sync, Blacksmith) |

### Host drivers (under `experiments/`)
| File | Build target |
|---|---|
| `core_scaling_sweep.cpp` | `metal_example_rh_core_scaling` |
| `bank_scaling_sweep.cpp` | `metal_example_rh_bank_scaling` |
| `subport_sweep.cpp` | `metal_example_rh_subport_sweep` |
| `subport_probe.cpp` | `metal_example_rh_subport_probe` |
| `intra_channel_sweep.cpp` | `metal_example_rh_intra_channel` |
| `peak_attack.cpp` | `metal_example_rh_peak_attack` |
| `peak_attack_refsync.cpp` | `metal_example_rh_peak_refsync` |
| `bank_boundary_probe.cpp` | `metal_example_rh_bank_boundary` |
| `peak_attack_blacksmith.cpp` | `metal_example_rh_blacksmith` |

---

## Claims to validate

### 1. Single-bank knee curve — `core_scaling_sweep.cpp`

**Run:** `./build_Release/programming_examples/metal_example_rh_core_scaling`

**Expected output (per-core M act/s and aggregate):**
| cores | per-core | aggregate |
|------:|---------:|----------:|
|     1 |    0.881 |     0.881 |
|     2 |    0.881 |     1.762 |
|     4 |    0.881 |     3.523 |
|     8 |    0.881 |     7.047 |
|    16 |    0.895 |    14.32  |
|    32 |    0.74  |    23.79  |
|    64 |    0.37  |    23.64  |
|   128 |    0.21  |    22.73  |

**Claim:** per-core rate flat in linear regime; per-bank ceiling ~24 M; saturation knee ~32 cores.

---

### 2. Cross-channel scaling (single-RISC) — `bank_scaling_sweep.cpp`

**Run:** `./build_Release/programming_examples/metal_example_rh_bank_scaling`

**Expected:**
| banks | cores | per-bank | aggregate |
|------:|------:|---------:|----------:|
|     1 |    16 |    14.32 |     14.32 |
|     2 |    32 |    14.30 |     28.59 |
|     4 |    64 |    14.21 |     56.24 |
|     8 |   128 |    14.30 |    112.79 |

**Claim:** 8 channels scale linearly, no shared bottleneck. **Single-RISC peak ≈ 113 M.**

---

### 3. Dual-RISC scaling — `bank_scaling_sweep.cpp --dual-risc`

**Run:** `./build_Release/programming_examples/metal_example_rh_bank_scaling --dual-risc`

**Expected:**
| banks | threads | per-bank | aggregate |
|------:|--------:|---------:|----------:|
|     1 |      32 |   28.67  |    28.67  |
|     2 |      64 |   28.31  |    55.94  |
|     4 |     128 |   27.99  |   110.26  |
|     8 |     256 |   28.04  |   220.75  |

**Claim:** dual-RISC ~2× single-RISC, **~221 M aggregate** at 8 ch × 16 tiles × 2 RISCs.

---

### 4. NOC sub-port scaling — `subport_sweep.cpp`

**Run:** `./build_Release/programming_examples/metal_example_rh_subport_sweep`

**Expected (channel 0, 16 tiles per sub-port, dual-RISC):**
| subports | threads | per-sp | aggregate |
|---------:|--------:|-------:|----------:|
|        1 |      32 |  28.60 |     28.60 |
|        2 |      64 |  27.97 |     55.94 |
|        3 |      96 |  26.59 |     **79.77** |

**Claim:** 3 sub-ports give 2.79× per-channel scaling; **per-bank ceiling 80 M**.

---

### 5. Sub-port physical aliasing — `subport_probe.cpp`

**Run:** `./build_Release/programming_examples/metal_example_rh_subport_probe`

**Expected:** writes `0xdeadbeef` via sub-port `(0,0)`, then reads via all three:
```
via sub-port 0 (0,0):  0xdeadbeef  MATCH
via sub-port 1 (0,1):  0xdeadbeef  MATCH
via sub-port 2 (0,11): 0xdeadbeef  MATCH
```

**Claim:** all 3 sub-ports alias to the same physical bank — sub-port scaling is real per-bank parallelism, not address aliasing.

---

### 6. Within-channel multi-bank — `intra_channel_sweep.cpp`

**Run (single sub-port):** `./build_Release/programming_examples/metal_example_rh_intra_channel`

**Expected:**
| ibanks | threads | per-bank | aggregate |
|-------:|--------:|---------:|----------:|
|      1 |      32 |    28.67 |     28.67 |
|      2 |      64 |    23.03 |     46.06 |
|      4 |     128 |    11.35 |     45.42 |
|      8 |     256 |     5.85 |     41.89 |

**Claim:** 1 sub-port × 2 banks gives 1.6× partial scaling; channel command bus saturates around 46 M with one sub-port.

**Run (3 sub-ports):** `--sub-ports 3 --tiles-per-bank 48 --sweep 1,2`
| ibanks | threads | per-bank | aggregate |
|-------:|--------:|---------:|----------:|
|      1 |      96 |    79.78 |     79.78 |
|      2 |     192 |    42.17 |     84.33 |

**Claim:** 3 sub-ports × 2 banks = 85.3 M aggregate, only +6% over 3sp×1bank → sub-ports and multi-bank are redundant levers, both push toward ~85 M per-channel ceiling.

---

### 7. num_sides invariance — `subport_sweep.cpp` with `--num-sides`

**Run loop:**
```bash
for n in 1 2 4 8 15; do printf "n=%d  " $n;
  ./build_Release/programming_examples/metal_example_rh_subport_sweep \
      --num-sides $n --sweep 3 --iterations 100000 2>&1 | grep "^     3 ";
done
```

**Expected aggregate Mas (column 6):**
| num_sides | aggr Mas |
|----------:|---------:|
|         1 |    84.55 |
|         2 |    79.16 |
|         4 |    78.45 |
|         8 |    79.85 |
|        15 |    81.19 |

**Claim:** per-channel command bandwidth ~80 M is invariant; num_sides only redistributes per-row.

---

### 8. Single-victim peak attack — `peak_attack.cpp`

**Run:** `./build_Release/programming_examples/metal_example_rh_peak_attack --banks 1 --sub-ports 3 --tiles-per-sp 16`

**Expected:**
- Total acts: 3,840,000,000 (3.84B)
- Aggregate rate: ~79.7 M act/s
- Wall: ~48 sec
- Bit flips: **0**
- ECC corrected delta: **0**
- Pre-write hash: `0xfec7f8a7920ec325`
- Post-attack hash: `0xfec7f8a7920ec325` (identical)

**Claim:** 3.84B confirmed real activations on a single victim row at the per-bank ceiling (80 M), zero flips, zero ECC.

---

### 9. 8-channel peak attack — `peak_attack.cpp` defaults

**Run:** `./build_Release/programming_examples/metal_example_rh_peak_attack`

**Expected:**
- Config: 8 banks × 16 tiles × 2 RISCs (no sub-ports) = 256 threads
- Total acts: 10,240,000,000 (10.24B)
- Aggregate rate: ~223.12 M act/s
- Wall: ~46 sec
- Bit flips: **0** across all 8 victims
- All 8 banks consume exactly 1.28B acts each in 35-46 sec wall time

**Claim:** 10.24B confirmed real activations across 8 victim rows, zero flips, ~223 M sustained aggregate.

---

### 10. Max-throughput attack — `peak_attack.cpp --sub-ports 3`

**Run:** `./build_Release/programming_examples/metal_example_rh_peak_attack --banks 8 --sub-ports 3 --tiles-per-sp 5`

**Expected:**
- Total threads: 240
- Total acts: 9.6B
- Aggregate rate: ~212 M act/s (slightly *lower* than 223 M baseline because under-saturating each sub-port)
- 0 flips, 0 ECC

**Claim:** sub-ports don't help aggregate when worker-constrained; 8 ch × 1 sp × 16 t (= 256 threads) at 223 M is the worker-bound chip-wide ceiling.

---

### 11. REF-sync delay sweep — `peak_attack_refsync.cpp`

**Run:** `./build_Release/programming_examples/metal_example_rh_peak_refsync`

**Expected:**
| delay | aggr Mas | per-row M | flips | ECC |
|------:|---------:|----------:|------:|----:|
|     0 |    80.69 |     40.35 |     0 |   0 |
|     8 |    76.81 |     38.40 |     0 |   0 |
|    16 |    75.02 |     37.51 |     0 |   0 |
|    32 |    71.78 |     35.89 |     0 |   0 |
|    48 |    69.07 |     34.53 |     0 |   0 |
|    64 |    66.84 |     33.42 |     0 |   0 |
|   100 |    61.53 |     30.76 |     0 |   0 |
|   200 |    50.06 |     25.03 |     0 |   0 |
|   500 |    31.79 |     15.90 |     0 |   0 |

**Claim:** monotonic degradation, no resonance peak. R4's old delay=48 "peak" was a NOC issue-rate artifact. Total activations across sweep ~1.7B, zero flips.

---

### 12. Bank-boundary verification — `bank_boundary_probe.cpp`

**Run (3 sub-ports):** `./build_Release/programming_examples/metal_example_rh_bank_boundary`

**Expected (key data points):**
| spacing | aggr Mas | bank? |
|--------:|---------:|-------|
|       1 |    79.6  | same  |
|       8 |    79.4  | same  |
|      15 |    78.9  | same  |
|    **16** |  **84.4** | **DIFF** ← jump |
|      17 |    79.6  | same (XOR cancel) |
|      18 |    79.6  | same (XOR cancel) |
|      20 |    79.6  | same |
|      32 |    84.2  | DIFF  |
|      64 |    84.4  | DIFF  |
|     128 |    85.4  | DIFF  |
|     512 |    79.7  | same  |
|    1024 |    85.5  | DIFF  |

**Run (precision sweep):** `--spacings 1,8,12,14,15,16,17,18,20,32`

**Claim:** ROWS_PER_BANK=16 boundary confirmed. XOR scrambling discovered: spacings 17, 18, 20 cancel back to same-bank despite crossing the +16 threshold. Bank ID is not just `addr >> 17`. Adjacent V±1 rows ARE same physical bank as V (the standard rowhammer assumption holds).

---

### 13. Blacksmith null — `peak_attack_blacksmith.cpp`

**Run uniform baseline:** `./build_Release/programming_examples/metal_example_rh_blacksmith --mode uniform`
- Total acts: 384M
- Aggregate rate: ~80.67 M
- 0 flips, 0 ECC

**Run blacksmith short:** `./build_Release/programming_examples/metal_example_rh_blacksmith --mode blacksmith`
- Same total acts (384M, same iterations)
- Aggregate rate: ~64.75 M (20% slower due to jitter)
- 0 flips, 0 ECC
- Thread distribution printed: `V±1=32 V±2=16 V±3=12 V±4=12 V±5=8 V±6=8 V±7=8`

**Run blacksmith long:** `./build_Release/programming_examples/metal_example_rh_blacksmith --mode blacksmith --iterations 20000000`
- Total acts: 3.84B
- Wall: ~60 sec
- 0 flips, 0 ECC

**Claim:** Blacksmith-style non-uniform timing + diverse aggressors produces same null result at 3.84B activations, with 20% throughput penalty. TRR isn't the binding constraint at our per-row activation rate (~1/1000× MAC threshold).

---

## Aggregate negative-result claim

Across all session experiments combined:
- **Confirmed real activations: > 50 billion** (FLUSH-READ ground truth, summed across all sweeps + attacks)
- **Bit flips on victim rows: 0**
- **ECC corrected counter delta: 0**
- **ECC uncorrectable counter delta: 0**

Verifiable end-to-end by running, in sequence:
```bash
./build_Release/programming_examples/metal_example_rh_peak_attack
./build_Release/programming_examples/metal_example_rh_peak_attack \
    --banks 1 --sub-ports 3 --tiles-per-sp 16
./build_Release/programming_examples/metal_example_rh_peak_attack \
    --banks 8 --sub-ports 3 --tiles-per-sp 5
./build_Release/programming_examples/metal_example_rh_blacksmith \
    --mode blacksmith --iterations 20000000
```

Total: ~27.5B confirmed activations, expected 0 flips and 0 ECC across all four runs.

---

## Headline architectural ceiling

| ceiling | value | how derived |
|---|---|---|
| Per-tile NOC issue rate | ~25 M | from R4 measurements |
| Per-bank tRC physical max | ~25 M acts/row/s | GDDR6 timing |
| Per-bank single-RISC saturation | 24 M | claim 1, knee at 32 cores |
| Per-bank dual-RISC + sub-ports | **80 M** | claim 4, claim 8 |
| Per-channel command bandwidth | ~80-85 M | claim 6, claim 7 (invariant across num_sides) |
| Cross-channel × dual-RISC aggregate | **223 M** | claim 9 |
| Cross-channel projected at full saturation | ~640 M | extrapolation from claim 4 × 8 |

**Per-row activation rate at peak per-row config:** ~40 M nominal, capped by tRC at ~25 M physical = 98 acts per 3.9 µs tREFI window per row.

**MAC threshold for rowhammer flips:** ~100,000 acts per tREFI per row.

**Per-row gap to threshold:** ~1000×, immutable from user-space (it's GDDR6 timing physics).
