// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// C2 — Pattern-sweep hammer kernel.  Same as the original double-sided
// kernel except the victim pattern and the aggressor pattern are independent
// runtime arguments.  This lets the host driver run the full
// (victim, aggressor) Cartesian product in a single sweep without having to
// rebuild the kernel — different cells are sensitive to different patterns,
// see Kim et al. ISCA14 and the Mutlu RowHammer Retrospective.
//
// Runtime args (positional):
//   0:  dram_noc_x          1: dram_noc_y
//   2:  victim_addr         3: hammer_iters
//   4:  victim_pattern      5: aggressor_pattern
//   6:  l1_scratch_addr     7: l1_result_addr

#include <cstdint>

constexpr uint32_t ROW_SIZE             = 8192;
constexpr uint32_t CACHELINE            = 64;
constexpr uint32_t CACHELINES_PER_ROW   = ROW_SIZE / CACHELINE;

constexpr uint32_t RESULT_HDR_WORDS     = 6;
constexpr uint32_t MAX_FLIP_RECORDS     = 32;
constexpr uint32_t FLIP_RECORD_WORDS    = 4;
constexpr uint32_t RESULT_BUF_WORDS     = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

void kernel_main() {
    uint32_t dram_noc_x       = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y       = get_arg_val<uint32_t>(1);
    uint32_t victim_addr      = get_arg_val<uint32_t>(2);
    uint32_t hammer_iters     = get_arg_val<uint32_t>(3);
    uint32_t victim_pattern   = get_arg_val<uint32_t>(4);
    uint32_t aggressor_pattern= get_arg_val<uint32_t>(5);
    uint32_t l1_scratch_addr  = get_arg_val<uint32_t>(6);
    uint32_t l1_result_addr   = get_arg_val<uint32_t>(7);

    uint32_t aggressor_lo = victim_addr - ROW_SIZE;
    uint32_t aggressor_hi = victim_addr + ROW_SIZE;
    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < RESULT_BUF_WORDS; ++i) results[i] = 0;

    // Phase 1: write patterns (victim and aggressor patterns are independent).
    {
        volatile uint32_t* fill_a = reinterpret_cast<volatile uint32_t*>(scratch_a);
        volatile uint32_t* fill_b = reinterpret_cast<volatile uint32_t*>(scratch_b);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); ++w) {
            fill_a[w] = victim_pattern;
            fill_b[w] = aggressor_pattern;
        }
        for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; ++cl) {
            uint64_t v_dst = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr  + cl * CACHELINE);
            uint64_t l_dst = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_lo + cl * CACHELINE);
            uint64_t h_dst = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_hi + cl * CACHELINE);
            noc_async_write(scratch_a, v_dst, CACHELINE);
            noc_async_write(scratch_b, l_dst, CACHELINE);
            noc_async_write(scratch_b, h_dst, CACHELINE);
        }
        noc_async_write_barrier();
    }

    uint64_t noc_lo = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_lo);
    uint64_t noc_hi = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_hi);

    // Phase 2: pipelined hammer.
    volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_start = (static_cast<uint64_t>(hi0) << 32) | lo0;

    for (uint32_t i = 0; i < hammer_iters; ++i) {
        noc_async_read(noc_lo, scratch_a, CACHELINE);
        noc_async_read(noc_hi, scratch_b, CACHELINE);
    }
    noc_async_read_barrier();

    volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_end = (static_cast<uint64_t>(hi1) << 32) | lo1;
    uint64_t elapsed = t_end - t_start;

    // Phase 3: verify.
    uint32_t num_flips = 0, total_bit_flips = 0, record_idx = 0;
    for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; ++cl) {
        uint64_t v_noc = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
        noc_async_read(v_noc, scratch_a, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t* rb = reinterpret_cast<volatile uint32_t*>(scratch_a);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); ++w) {
            uint32_t actual = rb[w];
            if (actual != victim_pattern) {
                ++num_flips;
                uint32_t diff = actual ^ victim_pattern;
                while (diff) { total_bit_flips += diff & 1u; diff >>= 1; }
                if (record_idx < MAX_FLIP_RECORDS) {
                    uint32_t base = RESULT_HDR_WORDS + record_idx * FLIP_RECORD_WORDS;
                    results[base + 0] = cl;
                    results[base + 1] = w;
                    results[base + 2] = victim_pattern;
                    results[base + 3] = actual;
                    ++record_idx;
                }
            }
        }
    }

    results[0] = 0;
    results[1] = num_flips;
    results[2] = total_bit_flips;
    results[3] = static_cast<uint32_t>(elapsed);
    results[4] = static_cast<uint32_t>(elapsed >> 32);
    results[5] = hammer_iters * 2;
}
