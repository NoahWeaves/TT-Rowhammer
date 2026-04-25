// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Geometry probe kernel — measures pairwise row-buffer access latency to
// reproduce the TT-Rowhammer characterization (Methods 2, 3, 5) on the
// CURRENT hardware.
//
// The host hands us a list of byte offsets (relative to the channel base)
// to probe.  For each pair (offset[2k], offset[2k+1]) we:
//
//   1. Issue an "open A then access B" sequence that forces a row-buffer
//      miss if A and B are on the same bank but different rows.  We use
//      the alternating A,B,A,B,... pattern with a final timed read of A,
//      same primitive used by TT-Rowhammer.
//   2. Record the median-style robust latency in cycles to the result
//      buffer at slot k.
//
// A single launch can probe up to MAX_PROBES pairs.  The host parses the
// resulting cycle counts and classifies each pair as same-row / row-buffer
// miss / cross-bank-group based on the 833 / 873 / 897 cycle bands.

#include <cstdint>

constexpr uint32_t CACHELINE     = 64;
constexpr uint32_t MAX_PROBES    = 64;     // pairs per kernel launch
constexpr uint32_t NUM_TRIALS    = 33;     // odd ⇒ exact median
constexpr uint32_t EVICT_BURST   = 16;     // 16 × 64 B = 1 KiB of B before timing A
constexpr uint32_t SCRATCH_BYTES = EVICT_BURST * CACHELINE; // per-side scratch budget

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
    uint32_t scratch_b = l1_scratch_addr + SCRATCH_BYTES;

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    // Layout: [0] = num_probes, [1+k] = median cycles for pair k.
    results[0] = num_probes;
    for (uint32_t k = 0; k < MAX_PROBES; ++k) results[1 + k] = 0xFFFFFFFFu;

    uint32_t samples[NUM_TRIALS];

    for (uint32_t k = 0; k < num_probes; ++k) {
        uint32_t off_a = get_arg_val<uint32_t>(5 + 2 * k + 0);
        uint32_t off_b = get_arg_val<uint32_t>(5 + 2 * k + 1);

        uint64_t noc_a = get_noc_addr(dram_noc_x, dram_noc_y, off_a);

        for (uint32_t t = 0; t < NUM_TRIALS; ++t) {
            // Warm-up: open A.
            noc_async_read(noc_a, scratch_a, CACHELINE);
            noc_async_read_barrier();

            // Force eviction: read EVICT_BURST distinct cachelines from row B.
            // If A and B share a bank, the controller must close A's row to
            // serve these.  EVICT_BURST cachelines = 1 KiB stays inside an
            // 8 KiB DRAM row so we never accidentally cross into row B+1.
            for (uint32_t j = 0; j < EVICT_BURST; ++j) {
                uint64_t noc_b = get_noc_addr(dram_noc_x, dram_noc_y,
                                              off_b + j * CACHELINE);
                noc_async_read(noc_b, scratch_b + j * CACHELINE, CACHELINE);
            }
            noc_async_read_barrier();

            // Time the re-access of A.  If A's row was closed, this pays
            // the row-activation cost; otherwise it's a row-buffer hit.
            volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
            uint64_t tA = (static_cast<uint64_t>(hi0) << 32) | lo0;

            noc_async_read(noc_a, scratch_a, CACHELINE);
            noc_async_read_barrier();

            volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
            uint64_t tB = (static_cast<uint64_t>(hi1) << 32) | lo1;

            samples[t] = static_cast<uint32_t>(tB - tA);
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
