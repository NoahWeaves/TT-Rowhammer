// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Geometry probe kernel — measures pairwise row-buffer access latency to
// reproduce the TT-Rowhammer characterization (Methods 2, 3, 5) on the
// CURRENT hardware.
//
// Protocol (matches `dram_latency/kernels/validation_probe.cpp` Mode 0):
//
//   For each (off_a, off_b) probe, repeat NUM_TRIALS times:
//     1. Flush-read at off_a + 128 MB to close any open row buffer.
//     2. Time a pipelined burst of BURST_PAIRS alternating (A, B) reads
//        with a SINGLE barrier at the end.
//     3. Sample = total cycles for the 2*BURST_PAIRS reads.
//   Result = median across trials, comparable to the canonical
//   833 / 873 / 897 cyc bands (same-row / diff-row / cross-bank-group).
//
// Why a pipelined burst and not a single timed read of A:
//   The previous version timed one A-read with a barrier-per-read, so the
//   ~456 cyc NOC round-trip dominated and masked the ~40 cyc DRAM row-switch
//   penalty. All bands collapsed to ~451 cyc and the latency tiers vanished.
//   A pipelined burst with one barrier at the end exposes the row-switch
//   cost because the controller's ability to overlap reads is exactly what
//   row conflicts disrupt.

#include <cstdint>

constexpr uint32_t CACHELINE     = 64;
constexpr uint32_t MAX_PROBES    = 64;     // pairs per kernel launch
constexpr uint32_t NUM_TRIALS    = 17;     // odd ⇒ exact median
constexpr uint32_t BURST_PAIRS   = 8;      // pipelined A,B,A,B,... pairs (matches canonical)
constexpr uint32_t FLUSH_OFFSET  = 128 * 1024 * 1024;  // 128 MB — toggles bit 27, crosses bank groups
constexpr uint32_t SCRATCH_BYTES = 2 * CACHELINE;       // one slot per side

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(2);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(3);
    uint32_t num_probes      = get_arg_val<uint32_t>(4);
    // Probes are pairs of byte offsets, packed as runtime args 5,6,7,8,...
    // num_probes pairs ⇒ 2 * num_probes runtime args follow.

    if (num_probes > MAX_PROBES) num_probes = MAX_PROBES;

    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    // Layout: [0] = num_probes, [1+k] = median total burst cycles for pair k.
    results[0] = num_probes;
    for (uint32_t k = 0; k < MAX_PROBES; ++k) results[1 + k] = 0xFFFFFFFFu;

    uint32_t samples[NUM_TRIALS];

    for (uint32_t k = 0; k < num_probes; ++k) {
        uint32_t off_a = get_arg_val<uint32_t>(5 + 2 * k + 0);
        uint32_t off_b = get_arg_val<uint32_t>(5 + 2 * k + 1);

        uint64_t noc_a     = get_noc_addr(dram_noc_x, dram_noc_y, off_a);
        uint64_t noc_b     = get_noc_addr(dram_noc_x, dram_noc_y, off_b);
        uint64_t noc_flush = get_noc_addr(dram_noc_x, dram_noc_y, off_a + FLUSH_OFFSET);

        for (uint32_t t = 0; t < NUM_TRIALS; ++t) {
            // Flush: read from a far region (bit 27 toggled) to close any
            // open row buffer this anchor's bank may still hold.
            noc_async_read(noc_flush, scratch_a, CACHELINE);
            noc_async_read_barrier();

            volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
            uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;

            // Pipelined alternating A/B burst, single barrier at end.
            for (uint32_t i = 0; i < BURST_PAIRS; ++i) {
                noc_async_read(noc_a, scratch_a, CACHELINE);
                noc_async_read(noc_b, scratch_b, CACHELINE);
            }
            noc_async_read_barrier();

            volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
            uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;

            samples[t] = static_cast<uint32_t>(t1 - t0);
        }

        // Insertion sort then pick the median.  NUM_TRIALS is small.
        for (uint32_t i = 1; i < NUM_TRIALS; ++i) {
            uint32_t v = samples[i];
            uint32_t j = i;
            while (j > 0 && samples[j - 1] > v) { samples[j] = samples[j - 1]; --j; }
            samples[j] = v;
        }
        results[1 + k] = samples[NUM_TRIALS / 2];
    }
}
