// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Latency-banded read probe.
//
// Issues SERIALIZED reads (barrier per read) cycling through n_rows distinct
// rows in the same bank. Records each read's individual latency and bins it
// into a 16-bucket histogram. The histogram directly counts row-buffer hits
// vs row-buffer misses (= real DRAM activations):
//
//   ~833 cyc band: same-row hit (no ACT)
//   ~873 cyc band: different-row miss (1 ACT)
//   ~897 cyc band: cross-bank-group miss (1 ACT, longer)
//   higher bands : contention with other in-flight requests on same bank
//
// At single-thread, queue depth is 1 (per-read barrier) so the controller
// cannot reorder. Every read in N≥2 alternation MUST be an ACT. The
// histogram should show ~100% of reads in the slow band, validating that
// per-bank ACT rate caps at 1/tRC physical limit.
//
// At multi-thread, contention skews latencies up. The hit/miss split is
// preserved relative to a single-thread baseline, but the absolute bin
// positions shift. Compare bin-shapes single vs multi to detect
// controller reordering at deep queues.
//
// Result layout (uint32 words):
//   [0..15]  histogram bin counts (see BIN_EDGES)
//   [16]     total_reads
//   [17]     total_cycles_lo
//   [18]     total_cycles_hi
//   [19]     niu_req_delta
//   [20]     niu_resp_delta
//   [21]     min_latency
//   [22]     max_latency
//   [23]     mode  (= 8)
//
// Runtime args:
//   arg 0:  dram_noc_x
//   arg 1:  dram_noc_y
//   arg 2:  base_addr (start of bank's row 0 within DRAM space)
//   arg 3:  num_reads (per-thread iterations, total reads issued)
//   arg 4:  n_rows (distinct rows to cycle through)
//   arg 5:  l1_scratch_addr (>= 1 cacheline)
//   arg 6:  l1_result_addr  (>= 24 uint32)

#include <cstdint>

constexpr uint32_t CACHELINE      = 64;
constexpr uint32_t ROW_SIZE       = 8192;
constexpr uint32_t MAX_N_ROWS     = 32;
constexpr uint32_t NUM_BINS       = 16;

// Bin upper bounds in BRISC cycles. Bins are 25-cyc-wide near the expected
// hit/miss bands (400-650 cyc) — observed measurements cluster there. ACT
// cost is ~40 cyc, so 25-cyc bins resolve hit-vs-miss bimodality.
constexpr uint32_t BIN_EDGES[NUM_BINS] = {
     400,   //  0: anomaly very fast
     425,   //  1: NOC RTT floor (~425)
     450,   //  2: hit zone center
     475,   //  3: hit zone high
     500,   //  4: transition
     525,   //  5: miss zone low
     550,   //  6: miss zone center (~550, NOC + ~75 ns DRAM)
     600,   //  7: miss zone high
     700,   //  8: light contention
     850,   //  9
    1000,   // 10
    1300,   // 11
    2000,   // 12
    3500,   // 13
    8000,   // 14
   0xFFFFFFFFu  // 15: catchall
};

constexpr uint32_t NIU_LOCAL_MST_RD_REQ_SENT      = 0xFFB20000 + 0x200 + 0x5 * 4;
constexpr uint32_t NIU_LOCAL_MST_RD_RESP_RECEIVED = 0xFFB20000 + 0x200 + 0x2 * 4;

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t base_addr       = get_arg_val<uint32_t>(2);
    uint32_t num_reads       = get_arg_val<uint32_t>(3);
    uint32_t n_rows          = get_arg_val<uint32_t>(4);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(5);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(6);

    if (n_rows == 0)         n_rows = 1;
    if (n_rows > MAX_N_ROWS) n_rows = MAX_N_ROWS;

    // Pre-compute NOC addresses for each row in the cycle.
    uint64_t row_noc[MAX_N_ROWS];
    for (uint32_t i = 0; i < n_rows; i++) {
        row_noc[i] = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + i * ROW_SIZE);
    }

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < 24; i++) results[i] = 0;
    results[23] = 8;  // mode tag

    // Warmup: open the row buffer at row 0 so first read isn't cold-start.
    noc_async_read(row_noc[0], l1_scratch_addr, CACHELINE);
    noc_async_read_barrier();

    // Snapshot NIU counters before timed loop.
    uint32_t niu_req_pre  = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_REQ_SENT);
    uint32_t niu_resp_pre = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_RESP_RECEIVED);

    uint32_t bins[NUM_BINS] = {0};
    uint32_t min_lat = 0xFFFFFFFFu;
    uint32_t max_lat = 0;

    volatile uint32_t* wcl = reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t* wch = reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

    uint32_t ts0_lo = *wcl;
    uint32_t ts0_hi = *wch;
    uint64_t t_start = (static_cast<uint64_t>(ts0_hi) << 32) | ts0_lo;

    for (uint32_t i = 0; i < num_reads; i++) {
        uint32_t idx = i % n_rows;
        uint32_t a_lo = *wcl;
        // Ignore upper word for per-read latency — read is at most a few µs.
        noc_async_read(row_noc[idx], l1_scratch_addr, CACHELINE);
        noc_async_read_barrier();
        uint32_t b_lo = *wcl;
        uint32_t latency = b_lo - a_lo;  // wraps cleanly for short reads

        // Bin search (linear, NUM_BINS=16 is small).
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
