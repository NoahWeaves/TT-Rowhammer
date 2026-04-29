// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Thread-stagger probe kernel.
//
// Same access pattern as latency_band_kernel (serialized barrier-per-read,
// cycling through n_rows distinct rows in same bank), but each thread waits
// a configurable delay before entering the timed loop. Driver supplies a
// per-thread `start_delay_cycles` so threads enter the hammer loop offset
// in time:
//
//   start_delay_cycles = thread_id × stagger_step
//
// Tests the controller's *temporal* reorder window: as stagger grows,
// instantaneous request overlap across threads drops, fewer same-row reqs
// can be batched into a single ACT, miss fraction should rise toward 100%
// and implied real ACT rate should approach the per-bank tRC ceiling.
//
// Result layout (uint32 words):
//   [0..15]  histogram bin counts (same bins as latency_band_kernel)
//   [16]     total_reads
//   [17]     total_cycles_lo
//   [18]     total_cycles_hi
//   [19]     niu_req_delta
//   [20]     niu_resp_delta
//   [21]     min_latency
//   [22]     max_latency
//   [23]     start_delay_cycles (echoed)
//
// Runtime args:
//   arg 0:  dram_noc_x
//   arg 1:  dram_noc_y
//   arg 2:  base_addr
//   arg 3:  num_reads (per-thread)
//   arg 4:  n_rows
//   arg 5:  l1_scratch_addr
//   arg 6:  l1_result_addr
//   arg 7:  start_delay_cycles  (busy-spin before timed loop)

#include <cstdint>

constexpr uint32_t CACHELINE      = 64;
constexpr uint32_t ROW_SIZE       = 8192;
constexpr uint32_t MAX_N_ROWS     = 32;
constexpr uint32_t NUM_BINS       = 16;

constexpr uint32_t BIN_EDGES[NUM_BINS] = {
     400, 425, 450, 475, 500, 525, 550, 600,
     700, 850, 1000, 1300, 2000, 3500, 8000, 0xFFFFFFFFu
};

constexpr uint32_t NIU_LOCAL_MST_RD_REQ_SENT      = 0xFFB20000 + 0x200 + 0x5 * 4;
constexpr uint32_t NIU_LOCAL_MST_RD_RESP_RECEIVED = 0xFFB20000 + 0x200 + 0x2 * 4;

void kernel_main() {
    uint32_t dram_noc_x        = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y        = get_arg_val<uint32_t>(1);
    uint32_t base_addr         = get_arg_val<uint32_t>(2);
    uint32_t num_reads         = get_arg_val<uint32_t>(3);
    uint32_t n_rows            = get_arg_val<uint32_t>(4);
    uint32_t l1_scratch_addr   = get_arg_val<uint32_t>(5);
    uint32_t l1_result_addr    = get_arg_val<uint32_t>(6);
    uint32_t start_delay_cyc   = get_arg_val<uint32_t>(7);

    if (n_rows == 0)         n_rows = 1;
    if (n_rows > MAX_N_ROWS) n_rows = MAX_N_ROWS;

    uint64_t row_noc[MAX_N_ROWS];
    for (uint32_t i = 0; i < n_rows; i++) {
        row_noc[i] = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + i * ROW_SIZE);
    }

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < 24; i++) results[i] = 0;
    results[23] = start_delay_cyc;

    volatile uint32_t* wcl = reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t* wch = reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

    // Warmup: open row buffer at row 0
    noc_async_read(row_noc[0], l1_scratch_addr, CACHELINE);
    noc_async_read_barrier();

    // Per-thread start-delay spin. All threads launch at roughly the same
    // wall-clock from host enqueue; we offset entry into the timed loop by
    // start_delay_cyc cycles. Use the wall_clock register so it scales with
    // BRISC's actual frequency.
    uint32_t delay_start = *wcl;
    while ((*wcl) - delay_start < start_delay_cyc) {
        // tight spin; wall_clock advances monotonically
    }

    uint32_t niu_req_pre  = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_REQ_SENT);
    uint32_t niu_resp_pre = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_RESP_RECEIVED);

    uint32_t bins[NUM_BINS] = {0};
    uint32_t min_lat = 0xFFFFFFFFu;
    uint32_t max_lat = 0;

    uint32_t ts0_lo = *wcl;
    uint32_t ts0_hi = *wch;
    uint64_t t_start = (static_cast<uint64_t>(ts0_hi) << 32) | ts0_lo;

    for (uint32_t i = 0; i < num_reads; i++) {
        uint32_t idx = i % n_rows;
        uint32_t a_lo = *wcl;
        noc_async_read(row_noc[idx], l1_scratch_addr, CACHELINE);
        noc_async_read_barrier();
        uint32_t b_lo = *wcl;
        uint32_t latency = b_lo - a_lo;

        for (uint32_t b = 0; b < NUM_BINS; b++) {
            if (latency < BIN_EDGES[b]) { bins[b]++; break; }
        }
        if (latency < min_lat) min_lat = latency;
        if (latency > max_lat) max_lat = latency;
    }

    uint32_t ts1_lo = *wcl;
    uint32_t ts1_hi = *wch;
    uint64_t t_end = (static_cast<uint64_t>(ts1_hi) << 32) | ts1_lo;
    uint64_t elapsed = t_end - t_start;

    uint32_t niu_req_post  = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_REQ_SENT);
    uint32_t niu_resp_post = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_RESP_RECEIVED);

    for (uint32_t b = 0; b < NUM_BINS; b++) results[b] = bins[b];
    results[16] = num_reads;
    results[17] = static_cast<uint32_t>(elapsed);
    results[18] = static_cast<uint32_t>(elapsed >> 32);
    results[19] = niu_req_post  - niu_req_pre;
    results[20] = niu_resp_post - niu_resp_pre;
    results[21] = (min_lat == 0xFFFFFFFFu) ? 0 : min_lat;
    results[22] = max_lat;
}
