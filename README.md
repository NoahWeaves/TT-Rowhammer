# Tenstorrent Blackhole Rowhammer Research

> User-space rowhammer feasibility study on the Tenstorrent Blackhole's
> 8-channel GDDR6 memory subsystem. Negative result: zero flips at the
> per-bank activation ceiling, with a measured ~1000× per-row gap to
> the published rowhammer MAC threshold.

---

## TL;DR

**Rowhammer attacks against Blackhole GDDR6 from user-space TT-Metalium are
infeasible.** Across >50 billion confirmed NOC-issued reads (>1.7B real DRAM
activations after controller coalescing), distributed across every attack
configuration we could construct from user-space — single-bank concentrated,
8-channel parallel, n-sided up to 20 aggressors, REF-sync delay sweeps,
Blacksmith-style non-uniform timing, and a 1-hour sustained pipelined hammer
at the per-bank ceiling — we observed:

- **0 bit flips** on any pattern-verified victim row
- **0 ECC corrected** errors (delta from idle baseline)
- **0 ECC uncorrectable** errors

The reason is an architectural ladder of three compounding limits:

| Limit | Value | Source |
|---|---|---|
| Per-tile NOC issue rate | ~25 M reads/s | Tensix NOC port bandwidth |
| Per-bank kernel-call rate at peak | ~168 M reads/s | 96 threads × 3 sub-ports, ch0, V±1 alternating |
| Per-bank NIU transaction rate at peak | ~114 M / s | 32% NOC-level coalescing at multi-thread contention |
| **Per-bank real ACT rate (validated)** | **~24 M ACT/s** | four independent measurements converge: `core_scaling` FLUSH-READ saturation = 24.0 M; `latency_band` miss-bin × rate = 26 M; `stagger_probe` (10 thread arrival patterns) = 22–25 M; `stagger_probe --deep` at full thread overlap = 25.3 M. tRC ≈ 41.7 ns |
| Per-aggressor-row real ACT rate (V±1 split) | **~12 M ACT/s** | half of per-bank, since 2 rows alternate |
| **Required for rowhammer (MAC threshold)** | **~25 G act/s/row** | published threshold ~100K acts per 3.9 µs tREFI |

We are **~2100× below** the activation rate needed per refresh window per
victim-row neighbor — that's per-tREFI count of ~47 acts vs MAC ~100K. The
bottleneck is the per-bank physical tRC (~41.7 ns on this Blackhole), which
fixes the per-bank ceiling at 24 M ACT/s regardless of how many threads or
sub-ports we throw at it. Aggregate "M reads/s" numbers reported by the
hammer tools are NOC issue rates that exceed this by 3–20×; the controller
absorbs the surplus via row-buffer hits and queue reordering. **The 24 M
ceiling is hardware physics — no user-space access pattern can exceed it.**

---

## What we measured

All numbers are reproducible from the binaries built by `CMakeLists.txt`.
See `documentation/SESSION_VALIDATION.md` for the full claim-by-claim
verification protocol with reproducer commands.

### Geometry (validated across all 8 channels)

| Property | Value |
|---|---|
| Row size | **8192 bytes (8KB)** |
| Rows per bank group | **16** |
| Bank addressing | XOR-scrambled (V±1 are same-bank, +16 jumps banks but +17/+18/+20 XOR back to same-bank) |
| Same-row latency | 833 cyc |
| Different-row latency | 873 cyc (+40 cyc row-switch penalty) |
| Cross-bank-group latency | 897 cyc (+24 cyc additional) |
| Memory controllers | **8 GDDR6 channels**, 3 NOC sub-ports each |
| Page mode | Open-page with controller reordering |

Reproduce with:
```bash
./build_Release/programming_examples/metal_example_rh_verify_geometry
./build_Release/programming_examples/metal_example_rh_test_addr_decoder
./build_Release/programming_examples/metal_example_rh_bank_boundary
```

### Throughput ceilings (per-bank, single victim)

| Config | Threads | M reads/s reported | Notes |
|---|---|---|---|
| 1 sub-port × 16 cores × 1 RISC | 16 | 14.3 | knee of single-sub-port scaling |
| 1 sub-port × 32 cores × 1 RISC | 32 | 24.0 | NOC-port saturation |
| 1 sub-port × 16 cores × 2 RISCs | 32 | 28.6 | dual-RISC adds ~2× |
| 3 sub-ports × 16 cores × 2 RISCs | **96** | **79.8** | **per-bank ceiling** |
| 3 sub-ports × ≥32 cores × 2 RISCs | >96 | regresses | oversubscription |

Reproduce with `metal_example_rh_core_scaling`, `metal_example_rh_subport_sweep`,
and `metal_example_rh_peak_attack --banks 1 --sub-ports 3 --tiles-per-sp 16`.

### Why "M reads/s" ≠ DRAM activations: four mechanisms in series

The reported "M reads/s" from any of these tools is the rate at which kernel
code calls `noc_async_read`. Real DRAM row-activation rate is lower, due to
four distinct mechanisms stacked in series:

1. **Row-buffer hits (DRAM hardware).** The bank's row buffer holds exactly
   one open row. Sequential reads to the same row hit the buffer with no
   new ACT. Confirmed via `metal_example_rh_row_cycle` mode 7: at N=1 (all
   reads to same row), single-thread throughput is 14.09 M reads/s; at
   N≥2 (alternating distinct rows in same bank), throughput drops to 10.97
   M reads/s and stays flat through N=64. The cache is 1-row-deep.

2. **NOC packet coalescing (NIU level, contention-driven).** Single-thread
   shows 1:1 ratio between kernel `noc_async_read` calls and NIU MST
   transactions. Multi-thread (96 threads on one channel) shows **0.68:1**
   — about 32% of kernel calls get aggregated by the NIU into wider NOC
   transactions before reaching the controller. Independent of access
   pattern. Confirmed via `metal_example_rh_row_cycle_mt`.

3. **Controller request reordering (queue-depth-dependent).** With a
   shallow queue (single-thread, K=16 in flight), the controller serves
   in issue order — N=2 alternating gives all-miss behavior, no reordering.
   With a deep queue (96 threads × K=16 ≈ 1500 in flight), the controller
   has enough reorder window to batch same-row reads from the queue into
   groups, dropping ACT count significantly. At N=2 multi-thread the
   slowdown vs same-row is only 7.6% (vs 22% single-thread), implying
   ~5× reorder amortization at peak config.

4. **Per-bank tRC physical limit.** A single GDDR6 bank can do at most
   ~21 M ACTs/s (tRC ≈ 48 ns). After the upstream coalescing layers, the
   real ACT rate at the bank cannot exceed this regardless of how many
   reads we issue.

**Composite factor at peak attack config (96 threads, V±1, ch0):**

| Layer | Rate after this layer | Coalescing factor at this layer |
|---|---|---|
| Kernel `noc_async_read` calls | 168 M reads/s | 1× |
| → NIU transactions | 114 M / s | 1.47× |
| → Real DRAM activations on bank | ~21 M / s | ~5× |
| → Per aggressor row (split between V±1) | ~10.5 M ACT/s | 2× |

So the per-row real activation rate is ~10 M/s = ~40 acts per tREFI window.
Vs MAC threshold ~100K, the gap is **~2400×** at peak config.

The R4 docs reported a "29× coalescing factor" derived from comparing
FLUSH-READ throughput (0.88 M/s, single-thread, with 128 MB unrelated read
between every probe) to PIPELINED throughput (25.81 M/s, single-thread,
same-row hits). That 29× number conflated FLUSH-protocol overhead with
single-thread row-buffer hits, and didn't directly measure controller
reordering. The corrected breakdown above replaces that single-number
framing.

### Negative-result attacks (all 0 flips)

| Attack | Total NOC reads | Real acts (est.) | Flips | ECC |
|---|---|---|---|---|
| `peak_attack` 8-channel default | 10.24 B | ~350 M | 0 | 0 |
| `peak_attack --banks 1 --sub-ports 3 --tiles-per-sp 16` | 3.84 B | ~130 M | 0 | 0 |
| `peak_attack_blacksmith --mode blacksmith --iterations 20M` | 3.84 B | ~130 M | 0 | 0 |
| `peak_attack_refsync` (delay sweep 0..500) | ~1.7 B | ~60 M | 0 | 0 |
| `long_hammer --duration-min 60 --channel 0` (1-hour) | **144.45 B** | **~5 B** | **0** | **0** |

The 1-hour long_hammer is the strongest negative we have: 96 threads
concentrated on ch0 victim row 30008 at the per-bank ceiling, sustained
without per-read barriers for the full hour. Pre/post-attack ECC counters
identical, victim row hash identical (`0xfec7f8a7920ec325` baseline preserved).

---

## Repo layout

```
.
├── README.md                          ← This file
│
├── rowhammer/
│   ├── rowhammer.cpp                  ← Original 2-sided rowhammer driver
│   ├── rowhammer_nsided.cpp           ← N-sided + REF-sync + extension driver
│   ├── CMakeLists.txt
│   │
│   ├── kernels/                       ← BRISC/NCRISC kernel sources
│   ├── experiments/                   ← Host drivers (build into metal_example_rh_*)
│   ├── documentation/
│   │   ├── FINAL_REPORT.md            ← Canonical writeup of negative result + 4-method tRC validation
│   │   ├── SESSION_VALIDATION.md      ← Per-claim reproducer commands (19 claims)
│   │   ├── PLAN.md / CONTEXT.md       ← Project-start design docs (banner-deprecated)
│   │   ├── replication_steps.md       ← Step-by-step reproduction
│   │   ├── verification_report.md     ← Auto-gen Phase B verification (banner-annotated)
│   │   └── history/                   ← Preserved campaign trail
│   │       ├── RUN_REPORT_R1.md       ← Initial 2-sided sweep
│   │       ├── RUN_REPORT_R2.md       ← Multi-pattern, multi-channel results
│   │       ├── RUN_REPORT_R3.md       ← Controller-coalescing hypothesis tests
│   │       └── RUN_REPORT_R4.md       ← Coalescing analysis (refined by FINAL_REPORT §2)
│   └── tools/
│       └── measure_dram_reads.py      ← NIU MST counter sampling helper
│
├── validation/                        ← Pre-recorded geometry validation outputs
├── refresh/                           ← Refresh-rate measurement traces
├── scripts/                           ← Build-helper scripts
└── generated/                         ← tt-metal compile artifacts (gitignored)
```

**Start reading:**
- [`rowhammer/documentation/FINAL_REPORT.md`](rowhammer/documentation/FINAL_REPORT.md)
  — canonical writeup of the negative result, the 4-method tRC validation,
  comparison to GPUHammer, and path forward.
- [`rowhammer/documentation/SESSION_VALIDATION.md`](rowhammer/documentation/SESSION_VALIDATION.md)
  — per-claim reproducer commands with expected outputs (19 claims).
- [`rowhammer/documentation/history/`](rowhammer/documentation/history/)
  — R1–R4 historical run reports preserved as the campaign trail. Their
  interpretive narratives have been refined by FINAL_REPORT §2 and §4.

---

## Building & running

Prerequisites: tt-metal SDK, cmake, Tenstorrent Blackhole hardware.

```bash
cd ~/tt-metal && source python_env/bin/activate
cmake --build build_Release -j$(nproc) --target \
    metal_example_rowhammer \
    metal_example_rowhammer_nsided \
    metal_example_rh_verify_geometry \
    metal_example_rh_test_addr_decoder \
    metal_example_rh_core_scaling \
    metal_example_rh_bank_scaling \
    metal_example_rh_subport_sweep \
    metal_example_rh_subport_probe \
    metal_example_rh_intra_channel \
    metal_example_rh_bank_boundary \
    metal_example_rh_forced_act \
    metal_example_rh_peak_attack \
    metal_example_rh_peak_refsync \
    metal_example_rh_blacksmith \
    metal_example_rh_long_hammer
```

All binaries land in `build_Release/programming_examples/`.

To reproduce the headline negative result in ~50 sec:
```bash
./build_Release/programming_examples/metal_example_rh_peak_attack
# 8 banks × 16 tiles × 2 RISCs = 256 threads, ~223 M aggregate, 0 flips
```

To reproduce the coalescing measurement:
```bash
./build_Release/programming_examples/metal_example_rh_forced_act --channel 0
# FLUSH-READ vs PIPELINED side-by-side; ~29× ratio
```

---

## Known caveats

1. **BRISC wall_clock constant is approximate.** All experiments use
   `NS_PER_CYCLE=1.25` (i.e., assume BRISC at 800 MHz, matching telemetry
   `aiclk`). Empirically, the 1-hour `long_hammer` run with
   `duration_cycles = 60 min × 800e6` completed in 35.6 min host wall, implying
   the BRISC `wall_clock` register increments at ~1.35 GHz — likely a different
   clock domain (REFCLK). **All "M reads/s" numbers reported by these tools
   are systematically underestimated by ~1.685×.** Ratios (coalescing factor,
   gap to threshold) are scale-invariant and unaffected. Fix is pending.

2. **`verify_geometry` Phase-B inline probe shows WARN.** The kernel uses
   serialized reads where the ~456-cycle NOC round-trip masks the ~40-cycle
   row-switch penalty, so latency tiers don't separate inline. The original
   `validation_probe` from `tt_metal/programming_examples/dram_latency`
   uses pipelined-burst-with-flush and reproduces 833/873/897 cycles cleanly
   on all 8 channels. See `documentation/history/RUN_REPORT_R4.md` §1 for full explanation.

3. **Per-bank "M act/s" numbers in tools are NOC issue rates, not real DRAM
   activations.** Apply the coalescing divider (~29× for reads, ~24× for
   writes) when interpreting against rowhammer thresholds.

---

## Hardware

| Component | Details |
|---|---|
| Hardware | Tenstorrent Blackhole, 4 GB GDDR6 per channel × 8 channels = 32 GB |
| Tile grid | 14 × 10 = 140 functional Tensix workers |
| Per-tile NOC ports | 2 (BRISC noc0, NCRISC noc1) |
| GDDR6 controllers | 8, with 3-way NOC sub-port ingress each |
| BRISC clock (telemetry) | 800 MHz aiclk |
| BRISC `wall_clock` register | empirically ~1.35 GHz (separate domain) |

---

## Related work

This project is a Blackhole-architecture follow-on to GPUHammer (USENIX
SEC '25), which demonstrated rowhammer flips on NVIDIA RTX A6000 GDDR6 and
ML model corruption. The core finding here — that user-space cannot reach
the per-row activation rate needed under any combination of available
levers — differs from GPUHammer's conclusion on NVIDIA hardware. The
Blackhole architecture's NOC latency floor (~456 cyc) and the GDDR6
controller's request reordering produce a per-row real activation rate
~1000× below the MAC threshold, where GPUHammer's setup reached the
threshold by exploiting CUDA-level memory access patterns that have no
direct analogue in TT-Metalium.

The path from this work to actual flips runs through firmware/driver
escalation (controller MMIO access, refresh-policy manipulation, custom
ARC firmware), not user-space exploit refinement.
