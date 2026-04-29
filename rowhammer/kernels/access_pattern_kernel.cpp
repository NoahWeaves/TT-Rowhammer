// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Access-pattern throughput probe.
//
// Runs K cacheline reads under one of several access patterns and reports
// total cycles. Comparing throughput across modes lets us infer whether the
// DRAM controller is satisfying reads from the row buffer (fast, NOC-bound)
// or actually issuing ACT commands (slower, tRC-bound).
//
// MODES (mode = arg 4):
//   0  same address V, K reads (no FLUSH)
//        → if row buffer hits, NOC-bound; if every read = ACT, tRC-bound
//   1  alternate V, V+64 (adjacent CL inside one 8KB region)
//        → adjacent CL should always row-buffer-hit
//   2  alternate V, V+8192 (one "row" away)
//        → if rows are physical adjacency, every other read = ACT
//        → if XOR scrambling decorrelates, behavior unclear
//   3  FLUSH-READ: read V+128MB then V (current "ground truth" pattern)
//        → if FLUSH closes V's row buffer, every V read = ACT
//   4  alternate V, V+128MB, V+256MB, V+384MB (definite multi-row, 4-way)
//        → every read should miss; tRC-bound across 4 distinct rows
//
// Runtime args:
//   arg 0:  dram_noc_x
//   arg 1:  dram_noc_y
//   arg 2:  base_addr
//   arg 3:  iterations  (K — total reads issued)
//   arg 4:  mode  (0..4)
//   arg 5:  l1_scratch_addr (>= 4 cachelines)
//   arg 6:  l1_result_addr  (4 uint32 — cycles_lo, cycles_hi, total_reads, mode)

#include <cstdint>

constexpr uint32_t CACHELINE     = 64;
constexpr uint32_t ROW_SIZE      = 8192;
constexpr uint32_t FLUSH_OFFSET  = 128u * 1024u * 1024u;

// Local-memory NIU master-side counters (each Tensix tile has its own NIU at
// 0xFFB20000). NIU_MST_RD_REQ_SENT is index 0x5; addr = base + 0x200 + 0x5*4.
constexpr uint32_t NIU_LOCAL_MST_RD_REQ_SENT      = 0xFFB20000 + 0x200 + 0x5 * 4;
constexpr uint32_t NIU_LOCAL_MST_RD_RESP_RECEIVED = 0xFFB20000 + 0x200 + 0x2 * 4;

constexpr uint32_t MAX_N_ROWS = 64;

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t base_addr       = get_arg_val<uint32_t>(2);
    uint32_t iterations      = get_arg_val<uint32_t>(3);
    uint32_t mode            = get_arg_val<uint32_t>(4);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(5);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(6);
    // arg 7 (mode 7 only): n_rows — cycle through N distinct rows
    uint32_t n_rows          = (mode == 7) ? get_arg_val<uint32_t>(7) : 1;
    if (n_rows == 0)         n_rows = 1;
    if (n_rows > MAX_N_ROWS) n_rows = MAX_N_ROWS;

    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;
    uint32_t scratch_c = l1_scratch_addr + 2 * CACHELINE;
    uint32_t scratch_d = l1_scratch_addr + 3 * CACHELINE;

    // Pre-compute up to 4 NOC addresses per mode (covers all patterns)
    uint64_t noc_a = get_noc_addr(dram_noc_x, dram_noc_y, base_addr);
    uint64_t noc_b, noc_c, noc_d;
    switch (mode) {
        case 1:  // adjacent CL
            noc_b = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + CACHELINE);
            noc_c = noc_a; noc_d = noc_a; break;
        case 2:  // adjacent "row"
            noc_b = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + ROW_SIZE);
            noc_c = noc_a; noc_d = noc_a; break;
        case 3:  // FLUSH-READ
            noc_b = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + FLUSH_OFFSET);
            noc_c = noc_a; noc_d = noc_a; break;
        case 4:  // 4-way far
            noc_b = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + FLUSH_OFFSET);
            noc_c = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + 2 * FLUSH_OFFSET);
            noc_d = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + 3 * FLUSH_OFFSET);
            break;
        case 6:  // pipelined FLUSH-READ — same flush addr as mode 3
            noc_b = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + FLUSH_OFFSET);
            noc_c = noc_a; noc_d = noc_a; break;
        default: // mode 0, 5
            noc_b = noc_a; noc_c = noc_a; noc_d = noc_a;
    }

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < 8; i++) results[i] = 0;
    results[3] = mode;

    // Warm-up: first read is unrepresentative (cold pipe). Issue one read
    // outside the timed loop to avoid skewing K-iter average.
    noc_async_read(noc_a, scratch_a, CACHELINE);
    noc_async_read_barrier();

    // Snapshot local NIU master-side counters BEFORE the timed loop
    uint32_t niu_req_pre  = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_REQ_SENT);
    uint32_t niu_resp_pre = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_RESP_RECEIVED);

    volatile uint32_t ts_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t ts_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_start = (static_cast<uint64_t>(ts_hi) << 32) | ts_lo;

    uint32_t total_reads = 0;
    if (mode == 0) {
        for (uint32_t k = 0; k < iterations; k++) {
            noc_async_read(noc_a, scratch_a, CACHELINE);
            noc_async_read_barrier();
        }
        total_reads = iterations;
    } else if (mode == 3) {
        // FLUSH-READ: read flush addr, then target. Pair counts as 2 reads
        // but only the second is a "real activation" by our convention.
        for (uint32_t k = 0; k < iterations; k++) {
            noc_async_read(noc_b, scratch_b, CACHELINE);  // flush
            noc_async_read_barrier();
            noc_async_read(noc_a, scratch_a, CACHELINE);  // target
            noc_async_read_barrier();
        }
        total_reads = 2 * iterations;
    } else if (mode == 4) {
        // 4-way far rotation
        uint32_t k_quarter = iterations / 4;
        for (uint32_t k = 0; k < k_quarter; k++) {
            noc_async_read(noc_a, scratch_a, CACHELINE);
            noc_async_read_barrier();
            noc_async_read(noc_b, scratch_b, CACHELINE);
            noc_async_read_barrier();
            noc_async_read(noc_c, scratch_c, CACHELINE);
            noc_async_read_barrier();
            noc_async_read(noc_d, scratch_d, CACHELINE);
            noc_async_read_barrier();
        }
        total_reads = 4 * k_quarter;
    } else if (mode == 5) {
        // Pipelined single-addr: K=16 reads issued back-to-back, one barrier
        // per group. Rotates scratch destination 8 ways to avoid L1 bank
        // serialization. Tests the DRAM-bound throughput when NOC pipe stays
        // full — same as refresh_probe mode 1 idiom.
        constexpr uint32_t K = 16;
        uint32_t outer = iterations / K;
        for (uint32_t s = 0; s < outer; s++) {
            for (uint32_t i = 0; i < K; i++) {
                noc_async_read(noc_a, scratch_a + ((i & 7) * CACHELINE), CACHELINE);
            }
            noc_async_read_barrier();
        }
        total_reads = outer * K;
    } else if (mode == 6) {
        // Pipelined FLUSH-READ: each iteration issues K flush+target pairs
        // back-to-back, then a single barrier. If FLUSH-READ does cause real
        // ACTs, the row-miss penalty (~50ns/access) becomes visible here
        // because NOC overhead is amortized away by pipelining.
        constexpr uint32_t K = 16;
        uint32_t outer = iterations / K;
        for (uint32_t s = 0; s < outer; s++) {
            for (uint32_t i = 0; i < K; i++) {
                noc_async_read(noc_b, scratch_b + ((i & 7) * CACHELINE), CACHELINE);
                noc_async_read(noc_a, scratch_a + ((i & 7) * CACHELINE), CACHELINE);
            }
            noc_async_read_barrier();
        }
        total_reads = 2 * outer * K;
    } else if (mode == 7) {
        // Pipelined N-row cycle: cycle reads through N distinct rows
        // (base_addr + i*ROW_SIZE for i=0..N-1). Tests when controller's
        // row-buffer cache is overflowed (cyc/read jumps from ~48 hit to
        // ~57 miss). The minimum N producing the jump = (cache depth + 1).
        uint64_t row_noc[MAX_N_ROWS];
        for (uint32_t i = 0; i < n_rows; i++) {
            row_noc[i] = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + i * ROW_SIZE);
        }
        constexpr uint32_t K = 16;
        uint32_t outer = iterations / K;
        uint32_t issued = 0;
        for (uint32_t s = 0; s < outer; s++) {
            for (uint32_t i = 0; i < K; i++) {
                uint32_t idx = (issued + i) % n_rows;
                noc_async_read(row_noc[idx], scratch_a + ((i & 7) * CACHELINE), CACHELINE);
            }
            noc_async_read_barrier();
            issued += K;
        }
        total_reads = outer * K;
    } else {
        // mode 1 or 2: alternate V and V'
        uint32_t k_half = iterations / 2;
        for (uint32_t k = 0; k < k_half; k++) {
            noc_async_read(noc_a, scratch_a, CACHELINE);
            noc_async_read_barrier();
            noc_async_read(noc_b, scratch_b, CACHELINE);
            noc_async_read_barrier();
        }
        total_reads = 2 * k_half;
    }

    volatile uint32_t te_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t te_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_end = (static_cast<uint64_t>(te_hi) << 32) | te_lo;
    uint64_t elapsed = t_end - t_start;

    // Snapshot local NIU counters AFTER the timed loop
    uint32_t niu_req_post  = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_REQ_SENT);
    uint32_t niu_resp_post = *reinterpret_cast<volatile uint32_t*>(NIU_LOCAL_MST_RD_RESP_RECEIVED);

    results[0] = static_cast<uint32_t>(elapsed);
    results[1] = static_cast<uint32_t>(elapsed >> 32);
    results[2] = total_reads;
    // results[3] = mode (set above)
    results[4] = niu_req_post  - niu_req_pre;   // NOC packets actually issued
    results[5] = niu_resp_post - niu_resp_pre;  // NOC read responses received
}
