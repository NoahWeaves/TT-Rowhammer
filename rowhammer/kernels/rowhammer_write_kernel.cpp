// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Write-based n-sided rowhammer kernel (technique C6).
//
// Reads can be coalesced from the GDDR6 row buffer; writes have to commit
// to the bank.  This kernel forces the controller to issue real ACT/WR/PRE
// sequences by hammering aggressors with noc_async_write instead of
// noc_async_read.  Same 3-phase structure as rowhammer_nsided_kernel:
// seed victim, hammer with writes, read back and count flips.
//
// Args (BRISC):
//   0:  dram_noc_x
//   1:  dram_noc_y
//   2:  victim_addr      DRAM byte offset of victim row
//   3:  hammer_iters     full sweeps over the aggressor list
//   4:  victim_pattern   uint32 written into victim row
//   5:  l1_scratch_addr  pair of cachelines (victim seed | aggressor pattern)
//   6:  l1_result_addr   header + flip records
//   7:  num_aggressors   1..30
//   8+: aggressor DRAM byte offsets

#include <cstdint>

constexpr uint32_t CACHELINE          = 64;
constexpr uint32_t ROW_SIZE           = 8192;
constexpr uint32_t CACHELINES_PER_ROW = ROW_SIZE / CACHELINE;
constexpr uint32_t MAX_AGGRESSORS     = 30;

constexpr uint32_t RESULT_HDR_WORDS   = 6;
constexpr uint32_t MAX_FLIP_RECORDS   = 32;
constexpr uint32_t FLIP_RECORD_WORDS  = 4;
constexpr uint32_t RESULT_BUF_WORDS   = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t victim_addr     = get_arg_val<uint32_t>(2);
    uint32_t hammer_iters    = get_arg_val<uint32_t>(3);
    uint32_t victim_pattern  = get_arg_val<uint32_t>(4);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(5);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(6);
    uint32_t num_aggressors  = get_arg_val<uint32_t>(7);

    if (num_aggressors > MAX_AGGRESSORS) num_aggressors = MAX_AGGRESSORS;

    uint32_t scratch_a = l1_scratch_addr;                // victim pattern source
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;    // aggressor pattern source

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < RESULT_BUF_WORDS; ++i) results[i] = 0;

    uint32_t aggressor_pattern = ~victim_pattern;
    {
        volatile uint32_t* fa = reinterpret_cast<volatile uint32_t*>(scratch_a);
        volatile uint32_t* fb = reinterpret_cast<volatile uint32_t*>(scratch_b);
        for (uint32_t w = 0; w < CACHELINE / 4; ++w) { fa[w] = victim_pattern; fb[w] = aggressor_pattern; }
    }

    uint32_t aggr_addrs[MAX_AGGRESSORS];
    uint64_t aggr_noc[MAX_AGGRESSORS];
    for (uint32_t i = 0; i < num_aggressors; ++i) {
        aggr_addrs[i] = get_arg_val<uint32_t>(8 + i);
        aggr_noc[i]   = get_noc_addr(dram_noc_x, dram_noc_y, aggr_addrs[i]);
    }

    // Phase 1: seed victim with the known pattern.
    for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; ++cl) {
        uint64_t dst = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
        noc_async_write(scratch_a, dst, CACHELINE);
    }
    noc_async_write_barrier();

    // Phase 2: hammer aggressors with WRITES.
    volatile uint32_t ts_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t ts_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_start = (static_cast<uint64_t>(ts_hi) << 32) | ts_lo;

    for (uint32_t k = 0; k < hammer_iters; ++k) {
        for (uint32_t a = 0; a < num_aggressors; ++a) {
            noc_async_write(scratch_b, aggr_noc[a], CACHELINE);
        }
    }
    noc_async_write_barrier();

    volatile uint32_t te_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t te_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_end = (static_cast<uint64_t>(te_hi) << 32) | te_lo;
    uint64_t elapsed = t_end - t_start;

    // Phase 3: read back victim, count flips.
    uint32_t num_flips = 0;
    uint32_t total_bit_flips = 0;
    uint32_t record_idx = 0;
    for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; ++cl) {
        uint64_t src = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
        noc_async_read(src, scratch_a, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t* rb = reinterpret_cast<volatile uint32_t*>(scratch_a);
        for (uint32_t w = 0; w < CACHELINE / 4; ++w) {
            uint32_t actual = rb[w];
            if (actual != victim_pattern) {
                ++num_flips;
                uint32_t diff = actual ^ victim_pattern;
                uint32_t bits = 0;
                while (diff) { bits += diff & 1; diff >>= 1; }
                total_bit_flips += bits;
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

    uint32_t total_acts = hammer_iters * num_aggressors;
    results[0] = 0;
    results[1] = num_flips;
    results[2] = total_bit_flips;
    results[3] = static_cast<uint32_t>(elapsed);
    results[4] = static_cast<uint32_t>(elapsed >> 32);
    results[5] = total_acts;
}
