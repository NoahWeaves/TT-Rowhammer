// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Bank conflict probe — finds the true same-bank stride by measuring
// pipelined burst throughput (not single-access latency).
//
// Key insight: the original validation_probe saw 833/873/897 cycle tiers
// using pipelined A-B bursts + 128MB flush.  We replicate that technique
// here, sweeping many strides to find which address pairs actually
// conflict in the same bank (not just same bank group).
//
// For each stride, we:
//   1. Flush with a +128MB read
//   2. Issue N pipelined A-B pairs (A = anchor, B = anchor + stride)
//   3. Barrier and measure total time
//   4. Report cycles per pair
//
// If A and B share a bank: row-buffer conflict → ~873 cyc/pair
// If A and B are in different banks: independent → ~833 cyc/pair
// If A and B cross bank groups: different bus → ~897 cyc/pair
//
// We sweep strides from 1×8KB to 64×8KB to find periodicity.
//
// === Runtime arguments ===
//   arg 0: dram_noc_x
//   arg 1: dram_noc_y
//   arg 2: l1_scratch_addr    (>= 256 bytes)
//   arg 3: l1_result_addr
//   arg 4: anchor_addr        safe DRAM address (8KB aligned)
//   arg 5: num_strides        how many strides to test
//   arg 6+: stride values in bytes (num_strides entries)

#include <cstdint>

constexpr uint32_t CACHELINE       = 64;
constexpr uint32_t FLUSH_OFFSET    = 128u * 1024u * 1024u;
constexpr uint32_t PAIRS_PER_BURST = 8;     // match validation_probe
constexpr uint32_t NUM_SAMPLES     = 32;    // trials per stride
constexpr uint32_t MAX_STRIDES     = 64;

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(2);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(3);
    uint32_t anchor_addr     = get_arg_val<uint32_t>(4);
    uint32_t num_strides     = get_arg_val<uint32_t>(5);

    if (num_strides > MAX_STRIDES) num_strides = MAX_STRIDES;

    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    // Layout: results[0] = num_strides, results[1+i] = median cycles/pair for stride i
    results[0] = num_strides;

    uint64_t noc_anchor = get_noc_addr(dram_noc_x, dram_noc_y, anchor_addr);
    uint64_t noc_flush  = get_noc_addr(dram_noc_x, dram_noc_y, anchor_addr + FLUSH_OFFSET);

    uint32_t samples[NUM_SAMPLES];

    for (uint32_t s = 0; s < num_strides; s++) {
        uint32_t stride = get_arg_val<uint32_t>(6 + s);
        uint64_t noc_b  = get_noc_addr(dram_noc_x, dram_noc_y, anchor_addr + stride);

        for (uint32_t trial = 0; trial < NUM_SAMPLES; trial++) {
            // Flush: close any open row
            noc_async_read(noc_flush, scratch_a, CACHELINE);
            noc_async_read_barrier();

            // Timed pipelined burst: PAIRS_PER_BURST × (A, B)
            volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

            for (uint32_t p = 0; p < PAIRS_PER_BURST; p++) {
                noc_async_read(noc_anchor, scratch_a, CACHELINE);
                noc_async_read(noc_b, scratch_b, CACHELINE);
            }
            noc_async_read_barrier();

            volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

            uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;
            uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;
            // Store raw total cycles for the burst (NOT divided)
            samples[trial] = static_cast<uint32_t>(t1 - t0);
        }

        // Insertion sort + median
        for (uint32_t i = 1; i < NUM_SAMPLES; i++) {
            uint32_t v = samples[i]; uint32_t j = i;
            while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
            samples[j] = v;
        }
        results[1 + s] = samples[NUM_SAMPLES / 2];
    }
}
