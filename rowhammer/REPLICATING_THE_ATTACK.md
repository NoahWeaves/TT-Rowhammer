# Replicating the Rowhammer Attack on Tenstorrent Blackhole

This guide walks through reproducing the rowhammer attacks contained in this
folder against the Blackhole GDDR6 DRAM. There are two attack programs:

| Binary | Source | What it does |
|---|---|---|
| `metal_example_rowhammer` | [rowhammer.cpp](rowhammer.cpp) + [kernels/rowhammer_kernel.cpp](kernels/rowhammer_kernel.cpp) | Classic **double-sided** hammer (aggressors at `V−1` and `V+1`) from a single Tensix core. |
| `metal_example_rowhammer_nsided` | [rowhammer_nsided.cpp](rowhammer_nsided.cpp) + [kernels/rowhammer_nsided_kernel.cpp](kernels/rowhammer_nsided_kernel.cpp) | **N-sided** TRR-evading hammer (up to 15 same-bank aggressors) launched on multiple Tensix cores in parallel. |

Both rely on the DRAM geometry characterised in [../README.md](../README.md):
8 KB row size, sequential address mapping, open-page mode. Aggressor rows for
victim row `V` are therefore at byte offsets `V·8192 ± 8192` (and ± `k·8192`
for N-sided).

---

## 1. Prerequisites

- Tenstorrent **Blackhole** card (the DRAM geometry constants in the kernels
  are Blackhole-specific — Wormhole has a different NOC layout and row size).
- `tt-metal` SDK checked out and built at `~/tt-metal` (or set `TT_METAL_HOME`).
- CMake ≥ 3.22, clang/gcc, Python 3.8+.
- `pyluwen` installed in the active Python environment (used by the host
  program to read GDDR6 ECC telemetry counters via
  `PciChip(pci_interface=0).get_telemetry()`). Without it the run still
  works, but ECC-corrected flips will be reported as "counters unavailable".

```bash
pip install pyluwen
python3 -c "from pyluwen import PciChip; print(PciChip(pci_interface=0).get_telemetry().gddr01_corr_errs)"
```

If the second command prints a number, ECC monitoring is wired up correctly.

---

## 2. Build

The CMake target lives in [CMakeLists.txt](CMakeLists.txt) and must be built
from the `tt-metal` build tree (it links `TT::Metalium`).

The simplest path is to drop / symlink this `rowhammer/` folder into
`~/tt-metal/tt_metal/programming_examples/` and add it via
`add_subdirectory(rowhammer)` in the parent `CMakeLists.txt`. Then:

```bash
cd ~/tt-metal
cmake --build build --target metal_example_rowhammer        -j$(nproc)
cmake --build build --target metal_example_rowhammer_nsided -j$(nproc)
```

The binaries land in `~/tt-metal/build/programming_examples/`.

---

## 3. Run the basic double-sided attack

```bash
cd ~/tt-metal
./build/programming_examples/metal_example_rowhammer --help
```

### CLI options ([rowhammer.cpp](rowhammer.cpp))

| Flag | Default | Meaning |
|---|---|---|
| `--channel N` | `0` | DRAM channel 0–7 to attack. |
| `--start-row N` | `32` | First victim row (skip the very first rows, they often hold metadata). |
| `--num-rows N` | `64` | Number of consecutive victim rows to sweep. |
| `--iterations N` | `500000` | Hammer iterations per victim (1 iteration = 2 activations). |
| `--pattern 0xNN` | `0x55555555` | 32-bit fill pattern written to the victim row before hammering. |
| `--all-channels` | off | Repeat the sweep against every DRAM channel (0–7). |
| `--barrier` | off | Use serialized NOC reads (slower but every read is a confirmed activation). |

### Recommended first run

A short smoke test that exercises one channel and prints ECC deltas:

```bash
./build/programming_examples/metal_example_rowhammer \
    --channel 0 --start-row 32 --num-rows 16 --iterations 500000
```

For each victim row the program:

1. Reads the GDDR6 ECC counters (before).
2. Writes `--pattern` into the victim row at `V·8192`.
3. Launches `kernels/rowhammer_kernel.cpp` on a Tensix core, which alternates
   NOC reads to `V−1` and `V+1` for `--iterations` rounds.
4. Reads the victim row back and diffs it against the original pattern.
5. Reads ECC counters (after) and prints any new corrected / uncorrectable
   errors per channel-pair.

**A successful attack manifests as one of:**

- A non-zero `*** ECC ... +N corrected ...` line — the on-die ECC silently
  fixed real bit flips (the most common outcome on hardened GDDR6).
- `+N uncorrectable` — multi-bit flips exceeded ECC.
- A flip record printed in the result-buffer dump — flips that bypassed ECC
  entirely.

If you only see `no new errors` across all rows, increase `--iterations`
(try 2 000 000, 5 000 000) and/or sweep more rows with `--num-rows`. A full
channel sweep is:

```bash
./build/programming_examples/metal_example_rowhammer \
    --all-channels --num-rows 256 --iterations 2000000
```

---

## 4. Run the N-sided / multi-core attack

This is the stronger attack — it is designed to overflow the DRAM's TRR
(Target Row Refresh) tracker by cycling through many same-bank aggressors,
and to multiply NOC pressure by hammering from several Tensix cores at once.

```bash
./build/programming_examples/metal_example_rowhammer_nsided --help
```

### CLI options ([rowhammer_nsided.cpp](rowhammer_nsided.cpp))

| Flag | Default | Meaning |
|---|---|---|
| `--channel N` | `0` | DRAM channel 0–7. |
| `--start-row N` | `1000` | First victim row. |
| `--num-rows N` | `16` | Victim rows to sweep. |
| `--iterations N` | `5000000` | Round-robin sweeps through the aggressor set per row. |
| `--pattern 0xNN` | `0x55555555` | Victim data pattern. |
| `--num-sides N` | `14` | Number of aggressor rows (2–15). Anything above the TRR table size (typically 1–16) is what evades mitigation. |
| `--num-cores N` | `4` | Tensix cores hammering in parallel (1–8). |
| `--barrier` | off | Serialized reads. |
| `--all-channels` | off | Repeat against every channel. |

### Recommended runs

Strong single-channel attempt (matches the defaults the program is tuned for):

```bash
./build/programming_examples/metal_example_rowhammer_nsided \
    --channel 0 --start-row 1000 --num-rows 16 \
    --num-sides 14 --num-cores 4 --iterations 5000000
```

Maximum pressure across all channels (long-running):

```bash
./build/programming_examples/metal_example_rowhammer_nsided \
    --all-channels --num-sides 14 --num-cores 8 --iterations 10000000
```

Sweep `--num-sides` to find the TRR break-point on your part:

```bash
for n in 2 4 6 8 10 12 14; do
  echo "=== num-sides=$n ==="
  ./build/programming_examples/metal_example_rowhammer_nsided \
      --channel 0 --num-sides $n --num-cores 4 \
      --num-rows 8 --iterations 2000000
done
```

The number of sides at which ECC `corrected` deltas suddenly jump is the
effective TRR table capacity for that channel.

---

## 5. Interpreting the output

Each row reports:

- **Hammer rate / activations per refresh window.** From the README baseline,
  a single core with pipelined reads delivers ~14–17 M activations/s, i.e.
  ~450 K–550 K activations per 32 ms refresh window. With `--num-cores 4`
  expect ~4× that. Typical TRH thresholds are 5 K–20 K, so the absolute rate
  is comfortably above the published threshold — bit flips are gated by ECC
  and TRR, not by raw rate.
- **ECC delta per channel pair** (`ch01`, `ch23`, `ch45`, `ch67`). The pair
  containing the channel under attack is the one to watch.
- **Flip records.** If a flip survived ECC, the kernel records the byte
  offset within the victim row, the expected vs. observed value, and the
  iteration count.

A clean run with no flips and no ECC deltas means either the part is fully
mitigated at the chosen parameters, or the aggressor pattern is not yet
strong enough — increase `--iterations`, `--num-sides`, or `--num-cores`.

---

## 6. Safety notes

- Hammering can corrupt **any** data sharing the targeted DRAM channel. Do
  not run these binaries on a card that is concurrently serving an inference
  workload or holding state you care about. A cold reset (`tt-smi -r 0`)
  restores the device.
- Uncorrectable ECC errors can put the GDDR6 controller into an error state.
  If subsequent runs hang or report NOC timeouts, reset the card.
- The defaults deliberately skip rows 0–31 because the lowest pages are
  often used by tt-metal for kernel scratch/argument storage.

---

## 7. Where to look next

- [../README.md](../README.md) — DRAM geometry and validation results.
- [../REPRODUCTION_GUIDE.md](../REPRODUCTION_GUIDE.md) — re-running the
  characterisation experiments that justify the 8 KB / sequential / open-page
  assumptions baked into the kernels.
- [kernels/rowhammer_kernel.cpp](kernels/rowhammer_kernel.cpp) and
  [kernels/rowhammer_nsided_kernel.cpp](kernels/rowhammer_nsided_kernel.cpp) —
  the on-device BRISC code that issues the actual NOC reads.
