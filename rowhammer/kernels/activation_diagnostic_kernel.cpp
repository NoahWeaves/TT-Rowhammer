// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Activation diagnostic kernel — determines whether NOC reads actually cause
// DRAM row activations or are being served from the row buffer / coalesced.
//
// Runs 5 micro-benchmarks, each with a barrier after every read so we get
// exact per-read cycle counts:
//
//   Test 0: SAME_ADDR    — read addr A repeatedly (should be row-buffer hit)
//   Test 1: SAME_ROW     — read two columns from the SAME row (should be hit)
//   Test 2: DIFF_ROW     — read from row A then row B in same bank (should be
//                          row-buffer miss = real activation)
//   Test 3: DIFF_BANK    — read from two different bank groups (cross-bank)
//   Test 4: PIPELINE_2   — issue A,B pipelined (no barrier between), then
//                          barrier. Measures whether pipelined reads to
//                          different rows still force real activations.
//
// Each test does TRIALS iterations and reports the median cycle count.
//
// === Result buffer layout ===
//   results[0]  = num_tests (5)
//   results[1]  = test 0 median cycles
//   results[2]  = test 1 median cycles
//   results[3]  = test 2 median cycles
//   results[4]  = test 3 median cycles
//   results[5]  = test 4 total cycles for TRIALS pipelined pairs
//   results[6]  = test 4 cycles per pair (total / TRIALS)
//
// === Runtime arguments ===
//   arg 0: dram_noc_x
//   arg 1: dram_noc_y
//   arg 2: l1_scratch_addr   (>= 256 bytes)
//   arg 3: l1_result_addr
//   arg 4: addr_a            DRAM byte address A (row X, some column)
//   arg 5: addr_a_col2       DRAM byte address A' (same row X, different column)
//   arg 6: addr_b            DRAM byte address B (different row Y, same bank)
//   arg 7: addr_c            DRAM byte address C (different bank group)

#include <cstdint>

constexpr uint32_t CACHELINE    = 64;
constexpr uint32_t TRIALS       = 64;
constexpr uint32_t NUM_TESTS    = 5;
constexpr uint32_t RESULT_WORDS = 1 + NUM_TESTS + 2;  // header + per-test + extras

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(2);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(3);
    uint32_t addr_a          = get_arg_val<uint32_t>(4);
    uint32_t addr_a_col2     = get_arg_val<uint32_t>(5);
    uint32_t addr_b          = get_arg_val<uint32_t>(6);
    uint32_t addr_c          = get_arg_val<uint32_t>(7);

    uint32_t scratch_0 = l1_scratch_addr;
    uint32_t scratch_1 = l1_scratch_addr + CACHELINE;

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < RESULT_WORDS; i++) results[i] = 0;
    results[0] = NUM_TESTS;

    uint64_t noc_a     = get_noc_addr(dram_noc_x, dram_noc_y, addr_a);
    uint64_t noc_a_c2  = get_noc_addr(dram_noc_x, dram_noc_y, addr_a_col2);
    uint64_t noc_b     = get_noc_addr(dram_noc_x, dram_noc_y, addr_b);
    uint64_t noc_c     = get_noc_addr(dram_noc_x, dram_noc_y, addr_c);

    uint32_t samples[TRIALS];

    // ── Helper: insertion sort + median ──────────────────────────────
    // Defined inline since we can't use lambdas easily on BRISC.

    // ═══════════════════════════════════════════════════════════════════
    // TEST 0: SAME_ADDR — read A, barrier, read A, barrier, time 2nd read
    // Should be a row-buffer HIT (~fast)
    // ═══════════════════════════════════════════════════════════════════
    for (uint32_t t = 0; t < TRIALS; t++) {
        // Warm: open row A
        noc_async_read(noc_a, scratch_0, CACHELINE);
        noc_async_read_barrier();

        // Time the re-read of same address
        volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        noc_async_read(noc_a, scratch_0, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

        uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;
        uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;
        samples[t] = static_cast<uint32_t>(t1 - t0);
    }
    // Sort + median
    for (uint32_t i = 1; i < TRIALS; i++) {
        uint32_t v = samples[i]; uint32_t j = i;
        while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
        samples[j] = v;
    }
    results[1] = samples[TRIALS / 2];

    // ═══════════════════════════════════════════════════════════════════
    // TEST 1: SAME_ROW — read A, barrier, read A' (different column), time A'
    // Should also be row-buffer HIT if open-page mode
    // ═══════════════════════════════════════════════════════════════════
    for (uint32_t t = 0; t < TRIALS; t++) {
        noc_async_read(noc_a, scratch_0, CACHELINE);
        noc_async_read_barrier();

        volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        noc_async_read(noc_a_c2, scratch_1, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

        uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;
        uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;
        samples[t] = static_cast<uint32_t>(t1 - t0);
    }
    for (uint32_t i = 1; i < TRIALS; i++) {
        uint32_t v = samples[i]; uint32_t j = i;
        while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
        samples[j] = v;
    }
    results[2] = samples[TRIALS / 2];

    // ═══════════════════════════════════════════════════════════════════
    // TEST 2: DIFF_ROW — read A, barrier, read B (different row, same bank)
    // Should be row-buffer MISS = real row activation (~slow)
    // ═══════════════════════════════════════════════════════════════════
    for (uint32_t t = 0; t < TRIALS; t++) {
        noc_async_read(noc_a, scratch_0, CACHELINE);
        noc_async_read_barrier();

        volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        noc_async_read(noc_b, scratch_1, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

        uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;
        uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;
        samples[t] = static_cast<uint32_t>(t1 - t0);
    }
    for (uint32_t i = 1; i < TRIALS; i++) {
        uint32_t v = samples[i]; uint32_t j = i;
        while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
        samples[j] = v;
    }
    results[3] = samples[TRIALS / 2];

    // ═══════════════════════════════════════════════════════════════════
    // TEST 3: DIFF_BANK — read A, barrier, read C (different bank group)
    // ═══════════════════════════════════════════════════════════════════
    for (uint32_t t = 0; t < TRIALS; t++) {
        noc_async_read(noc_a, scratch_0, CACHELINE);
        noc_async_read_barrier();

        volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        noc_async_read(noc_c, scratch_1, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

        uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;
        uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;
        samples[t] = static_cast<uint32_t>(t1 - t0);
    }
    for (uint32_t i = 1; i < TRIALS; i++) {
        uint32_t v = samples[i]; uint32_t j = i;
        while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
        samples[j] = v;
    }
    results[4] = samples[TRIALS / 2];

    // ═══════════════════════════════════════════════════════════════════
    // TEST 4: PIPELINED PAIR — issue A,B back-to-back (no barrier between),
    // then barrier. This is what the attack actually does.
    // Measure total wall time for TRIALS pairs.
    // ═══════════════════════════════════════════════════════════════════
    {
        volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;

        for (uint32_t t = 0; t < TRIALS; t++) {
            noc_async_read(noc_a, scratch_0, CACHELINE);
            noc_async_read(noc_b, scratch_1, CACHELINE);
        }
        noc_async_read_barrier();

        volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;

        uint32_t total = static_cast<uint32_t>(t1 - t0);
        results[5] = total;
        results[6] = total / TRIALS;  // cycles per A,B pair
    }

    // ═══════════════════════════════════════════════════════════════════
    // TEST 5: PIPELINED SAME_ADDR — issue A,A back-to-back (should coalesce)
    // Compare against test 4 to see if pipelining different rows is slower.
    // ═══════════════════════════════════════════════════════════════════
    {
        volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;

        for (uint32_t t = 0; t < TRIALS; t++) {
            noc_async_read(noc_a, scratch_0, CACHELINE);
            noc_async_read(noc_a, scratch_1, CACHELINE);
        }
        noc_async_read_barrier();

        volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;

        uint32_t total = static_cast<uint32_t>(t1 - t0);
        results[7] = total;
        results[8] = total / TRIALS;  // cycles per A,A pair
    }
}
