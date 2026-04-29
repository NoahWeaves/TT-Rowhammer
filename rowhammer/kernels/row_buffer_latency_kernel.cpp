// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Row-buffer latency probe kernel.
//
// Goal: discriminate which DRAM addresses physically alias to the same row
// (row-buffer hit, fast) vs distinct rows (row miss = ACT + RD, slow).
// Methodology mirrors GPUHammer's address-mapping reverse-engineering.
//
// For each (base, target) pair the kernel runs SAMPLES_PER_OFFSET iterations:
//   1. warm: read `base` twice → row buffer for base's physical row is open
//   2. timed: read `target` with cycle-precise WALL_CLOCK timing
//      → if target aliases base's row, latency ≈ tCL + NOC
//      → if target is a different row, latency ≈ tRP + tRCD + tCL + NOC
//
// We accumulate per-offset min, max, and sum so the host can compute
// avg/min/max latency in cycles and infer hit/miss clustering.
//
// Runtime args:
//   arg 0:  dram_noc_x
//   arg 1:  dram_noc_y
//   arg 2:  base_addr           (warm address — opens row buffer)
//   arg 3:  l1_scratch_addr     (>= 2 cachelines)
//   arg 4:  l1_result_addr
//   arg 5:  num_offsets
//   arg 6:  samples_per_offset
//   arg 7:  flush_addr          (address to read between samples to evict)
//   arg 8+: target addresses (num_offsets × uint32_t)
//
// Result layout: per offset, 3 uint32 (sum_lo, min, max).
// Total result words = num_offsets * 3.

#include <cstdint>

constexpr uint32_t CACHELINE        = 64;
constexpr uint32_t MAX_OFFSETS      = 64;

void kernel_main() {
    uint32_t dram_noc_x        = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y        = get_arg_val<uint32_t>(1);
    uint32_t base_addr         = get_arg_val<uint32_t>(2);
    uint32_t l1_scratch_addr   = get_arg_val<uint32_t>(3);
    uint32_t l1_result_addr    = get_arg_val<uint32_t>(4);
    uint32_t num_offsets       = get_arg_val<uint32_t>(5);
    uint32_t samples_per_off   = get_arg_val<uint32_t>(6);
    uint32_t flush_addr        = get_arg_val<uint32_t>(7);

    if (num_offsets > MAX_OFFSETS) num_offsets = MAX_OFFSETS;

    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;

    uint32_t target_addrs[MAX_OFFSETS];
    uint64_t target_noc[MAX_OFFSETS];
    for (uint32_t i = 0; i < num_offsets; i++) {
        target_addrs[i] = get_arg_val<uint32_t>(8 + i);
        target_noc[i]   = get_noc_addr(dram_noc_x, dram_noc_y, target_addrs[i]);
    }
    uint64_t base_noc  = get_noc_addr(dram_noc_x, dram_noc_y, base_addr);
    uint64_t flush_noc = get_noc_addr(dram_noc_x, dram_noc_y, flush_addr);

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < num_offsets * 3; i++) results[i] = 0;

    for (uint32_t i = 0; i < num_offsets; i++) {
        uint64_t sum_cyc  = 0;
        uint32_t min_cyc  = 0xFFFFFFFFu;
        uint32_t max_cyc  = 0;

        for (uint32_t s = 0; s < samples_per_off; s++) {
            // Evict any stale row buffer state by hitting a far-away address
            noc_async_read(flush_noc, scratch_b, CACHELINE);
            noc_async_read_barrier();

            // Warm the base row buffer — TWO reads to ensure it's open
            noc_async_read(base_noc, scratch_a, CACHELINE);
            noc_async_read_barrier();
            noc_async_read(base_noc, scratch_a, CACHELINE);
            noc_async_read_barrier();

            // Timed read of the target offset
            volatile uint32_t t0_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            noc_async_read(target_noc[i], scratch_b, CACHELINE);
            noc_async_read_barrier();
            volatile uint32_t t1_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);

            uint32_t cyc = t1_lo - t0_lo;
            sum_cyc += cyc;
            if (cyc < min_cyc) min_cyc = cyc;
            if (cyc > max_cyc) max_cyc = cyc;
        }

        // Average cycles (truncated)
        uint32_t avg_cyc = static_cast<uint32_t>(sum_cyc / samples_per_off);
        results[i * 3 + 0] = avg_cyc;
        results[i * 3 + 1] = min_cyc;
        results[i * 3 + 2] = max_cyc;
    }
}
