// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Long-run pipelined hammer kernel.
//
// Time-bounded: kernel exits when wall_clock - t_start exceeds duration_cycles.
// Pipelined K=16 reads alternating through num_aggressors addresses with one
// barrier per K (no per-read barrier — NOC pipe stays full → DRAM-bound).
//
// Per-bank primary thread (core_id == 0):
//   - PHASE 1: writes test pattern to victim row + opposite pattern to all
//              aggressor rows
//   - PHASE 2: hammers
//   - PHASE 3: verifies victim row, counts flips and bit-flips
//
// All other threads: hammer-only.
//
// Result layout (uint32 words):
//   [0]  iters_done  (lower 32 bits — kernel may run > 4G outer iters)
//   [1]  iters_done_hi
//   [2]  total_acts_lo
//   [3]  total_acts_hi
//   [4]  elapsed_cycles_lo
//   [5]  elapsed_cycles_hi
//   [6]  niu_req_delta
//   [7]  niu_resp_delta
//   [8]  num_flipped_cachelines  (primary only)
//   [9]  total_bit_flips         (primary only)
//   [10] flip_record_count
//   [11] reserved
//   [12+] flip records: (cl_idx, word_idx, expected, actual) × MAX_FLIP_RECORDS
//
// Runtime args:
//   arg 0:  dram_noc_x
//   arg 1:  dram_noc_y
//   arg 2:  victim_addr
//   arg 3:  duration_cycles_lo
//   arg 4:  duration_cycles_hi
//   arg 5:  data_pattern
//   arg 6:  l1_scratch_addr (>= 16 cachelines)
//   arg 7:  l1_result_addr
//   arg 8:  core_id  (0 = primary)
//   arg 9:  num_aggressors
//   arg 10+: aggressor DRAM addresses

#include <cstdint>

constexpr uint32_t ROW_SIZE           = 8192;
constexpr uint32_t CACHELINE          = 64;
constexpr uint32_t CACHELINES_PER_ROW = ROW_SIZE / CACHELINE;
constexpr uint32_t MAX_AGGRESSORS     = 16;
constexpr uint32_t BURST_K            = 16;
constexpr uint32_t TIME_CHECK_MASK    = 0xFFF;  // check time every 4096 outer iters

constexpr uint32_t RESULT_HDR_WORDS   = 12;
constexpr uint32_t MAX_FLIP_RECORDS   = 32;
constexpr uint32_t FLIP_RECORD_WORDS  = 4;

constexpr uint32_t NIU_LOCAL_MST_RD_REQ_SENT      = 0xFFB20000 + 0x200 + 0x5 * 4;
constexpr uint32_t NIU_LOCAL_MST_RD_RESP_RECEIVED = 0xFFB20000 + 0x200 + 0x2 * 4;

inline uint64_t wall_clock_64() {
    uint32_t lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    uint32_t hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void kernel_main() {
    uint32_t dram_noc_x        = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y        = get_arg_val<uint32_t>(1);
    uint32_t victim_addr       = get_arg_val<uint32_t>(2);
    uint32_t duration_cyc_lo   = get_arg_val<uint32_t>(3);
    uint32_t duration_cyc_hi   = get_arg_val<uint32_t>(4);
    uint32_t data_pattern      = get_arg_val<uint32_t>(5);
    uint32_t l1_scratch_addr   = get_arg_val<uint32_t>(6);
    uint32_t l1_result_addr    = get_arg_val<uint32_t>(7);
    uint32_t core_id           = get_arg_val<uint32_t>(8);
    uint32_t num_aggressors    = get_arg_val<uint32_t>(9);

    if (num_aggressors > MAX_AGGRESSORS) num_aggressors = MAX_AGGRESSORS;
    if (num_aggressors < 1) num_aggressors = 1;
    uint64_t duration_cycles = (static_cast<uint64_t>(duration_cyc_hi) << 32) | duration_cyc_lo;

    uint32_t scratch_pattern = l1_scratch_addr;
    uint32_t scratch_anti    = l1_scratch_addr + CACHELINE;
    uint32_t scratch_burst   = l1_scratch_addr + 2 * CACHELINE;  // 8 CLs for K=16 burst rotation

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    uint32_t result_buf_words = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
    for (uint32_t i = 0; i < result_buf_words; i++) results[i] = 0;

    uint32_t aggr_addrs[MAX_AGGRESSORS];
    uint64_t aggr_noc[MAX_AGGRESSORS];
    for (uint32_t i = 0; i < num_aggressors; i++) {
        aggr_addrs[i] = get_arg_val<uint32_t>(10 + i);
        aggr_noc[i]   = get_noc_addr(dram_noc_x, dram_noc_y, aggr_addrs[i]);
    }

    // ─── PHASE 1 (primary only): seed victim and aggressors ────────────
    if (core_id == 0) {
        volatile uint32_t* fp = reinterpret_cast<volatile uint32_t*>(scratch_pattern);
        volatile uint32_t* fa = reinterpret_cast<volatile uint32_t*>(scratch_anti);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
            fp[w] = data_pattern;
            fa[w] = ~data_pattern;
        }
        // Write pattern to victim
        for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
            uint64_t dst = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
            noc_async_write(scratch_pattern, dst, CACHELINE);
        }
        noc_async_write_barrier();
        // Write anti-pattern to all aggressors
        for (uint32_t a = 0; a < num_aggressors; a++) {
            for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
                uint64_t dst = get_noc_addr(dram_noc_x, dram_noc_y,
                                            aggr_addrs[a] + cl * CACHELINE);
                noc_async_write(scratch_anti, dst, CACHELINE);
            }
        }
        noc_async_write_barrier();
    }

    // ─── PHASE 2: time-bounded pipelined hammer ────────────────────────
    uint32_t niu_req_pre  = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_REQ_SENT);
    uint32_t niu_resp_pre = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_RESP_RECEIVED);

    uint64_t t_start = wall_clock_64();
    uint64_t t_end_target = t_start + duration_cycles;

    uint64_t iters_done = 0;
    uint64_t total_acts = 0;

    while (true) {
        // Periodic time check + checkpoint
        if ((static_cast<uint32_t>(iters_done) & TIME_CHECK_MASK) == 0) {
            uint64_t now = wall_clock_64();
            if (now >= t_end_target) break;
            // Checkpoint to L1 so host can read partial progress
            results[0] = static_cast<uint32_t>(iters_done);
            results[1] = static_cast<uint32_t>(iters_done >> 32);
            results[2] = static_cast<uint32_t>(total_acts);
            results[3] = static_cast<uint32_t>(total_acts >> 32);
            uint64_t elapsed = now - t_start;
            results[4] = static_cast<uint32_t>(elapsed);
            results[5] = static_cast<uint32_t>(elapsed >> 32);
        }
        // K=16 pipelined reads cycling through aggressors
        for (uint32_t i = 0; i < BURST_K; i++) {
            uint32_t idx = i % num_aggressors;
            noc_async_read(aggr_noc[idx], scratch_burst + ((i & 7) * CACHELINE), CACHELINE);
        }
        noc_async_read_barrier();
        iters_done++;
        total_acts += BURST_K;
    }

    uint64_t t_now = wall_clock_64();
    uint64_t elapsed = t_now - t_start;
    uint32_t niu_req_post  = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_REQ_SENT);
    uint32_t niu_resp_post = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_RESP_RECEIVED);

    results[0] = static_cast<uint32_t>(iters_done);
    results[1] = static_cast<uint32_t>(iters_done >> 32);
    results[2] = static_cast<uint32_t>(total_acts);
    results[3] = static_cast<uint32_t>(total_acts >> 32);
    results[4] = static_cast<uint32_t>(elapsed);
    results[5] = static_cast<uint32_t>(elapsed >> 32);
    results[6] = niu_req_post  - niu_req_pre;
    results[7] = niu_resp_post - niu_resp_pre;

    // ─── PHASE 3 (primary only): verify victim row ─────────────────────
    if (core_id == 0) {
        uint32_t num_flipped_cl = 0;
        uint32_t total_bit_flips = 0;
        uint32_t record_idx = 0;

        for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
            uint64_t v_noc = get_noc_addr(dram_noc_x, dram_noc_y,
                                          victim_addr + cl * CACHELINE);
            noc_async_read(v_noc, scratch_pattern, CACHELINE);
            noc_async_read_barrier();

            volatile uint32_t* rb = reinterpret_cast<volatile uint32_t*>(scratch_pattern);
            for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
                uint32_t actual = rb[w];
                if (actual != data_pattern) {
                    num_flipped_cl++;
                    uint32_t diff = actual ^ data_pattern;
                    while (diff) { total_bit_flips += diff & 1; diff >>= 1; }

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
        results[8]  = num_flipped_cl;
        results[9]  = total_bit_flips;
        results[10] = record_idx;
    }
}
