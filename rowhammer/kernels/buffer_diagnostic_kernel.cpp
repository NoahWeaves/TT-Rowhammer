// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Buffer layer diagnostic — systematically identifies what's between the NOC
// and the DRAM cells that absorbs row-switch penalties.
//
// === TESTS ===
//
// SECTION A: Data provenance (do reads actually hit DRAM?)
//   Write distinct patterns to N rows, read them back, verify we get the
//   correct unique data.  If YES: reads reach DRAM, the issue is the
//   controller hiding latency.  If NO: some cache is serving stale data.
//
// SECTION B: Multi-bank-buffer saturation
//   GDDR6 has up to 16 banks with independent row buffers.  Open rows in
//   K banks (K = 1..16), then time a re-access to the FIRST row.  If
//   latency jumps at some K, that's the number of row buffers.
//
// SECTION C: Large-stride sweep
//   Time (anchor, anchor + stride) for strides from 8KB to 8MB in
//   powers of 2.  If the bank mapping uses XOR or higher address bits,
//   we'll find the real bank-conflict stride.
//
// SECTION D: Write-read vs read-read comparison
//   Write to addr B then read addr A (forces DRAM commit on B).
//   Compare latency to read B then read A.
//
// === Result buffer layout ===
//   [0]       = magic (0xD1A6)
//   [1..16]   = Section A: pattern verification (1=pass per row, 0=fail)
//   [17]      = Section A: num_rows_tested
//   [18]      = Section A: num_rows_correct
//   [19..34]  = Section B: latency for K=1..16 open rows before re-access
//   [35..50]  = Section C: latency for stride = 8KB, 16KB, ..., 8MB (16 entries)
//   [51]      = Section D: read-read latency (A after B)
//   [52]      = Section D: write-read latency (A after write-B)
//
// === Runtime arguments ===
//   arg 0: dram_noc_x
//   arg 1: dram_noc_y
//   arg 2: l1_scratch_addr  (>= 512 bytes)
//   arg 3: l1_result_addr
//   arg 4: base_addr        (safe DRAM start, 8KB aligned, with 8MB+ headroom)

#include <cstdint>

constexpr uint32_t ROW_SIZE      = 8192;
constexpr uint32_t CACHELINE     = 64;
constexpr uint32_t NUM_ROWS_TEST = 16;
constexpr uint32_t TIMING_TRIALS = 33;
constexpr uint32_t RESULT_WORDS  = 64;

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(2);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(3);
    uint32_t base_addr       = get_arg_val<uint32_t>(4);

    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;
    uint32_t scratch_c = l1_scratch_addr + 2 * CACHELINE;  // for write pattern

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < RESULT_WORDS; i++) results[i] = 0;
    results[0] = 0xD1A6;  // magic

    uint32_t samples[TIMING_TRIALS];

    // ═══════════════════════════════════════════════════════════════════
    // SECTION A: Data provenance — do reads actually return DRAM content?
    // ═══════════════════════════════════════════════════════════════════
    // Write a unique pattern (row_index * 0x01010101) to the first cacheline
    // of each of 16 consecutive rows, then read them back.

    // Phase A1: Write unique patterns
    volatile uint32_t* wb = reinterpret_cast<volatile uint32_t*>(scratch_c);
    for (uint32_t r = 0; r < NUM_ROWS_TEST; r++) {
        uint32_t pattern = (r + 1) * 0x01010101u;  // row 0→0x01010101, row 1→0x02020202, etc
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
            wb[w] = pattern;
        }
        uint64_t dst = get_noc_addr(dram_noc_x, dram_noc_y,
                                     base_addr + r * ROW_SIZE);
        noc_async_write(scratch_c, dst, CACHELINE);
    }
    noc_async_write_barrier();

    // Phase A2: Read back and verify
    uint32_t correct = 0;
    for (uint32_t r = 0; r < NUM_ROWS_TEST; r++) {
        uint64_t src = get_noc_addr(dram_noc_x, dram_noc_y,
                                     base_addr + r * ROW_SIZE);
        noc_async_read(src, scratch_a, CACHELINE);
        noc_async_read_barrier();

        volatile uint32_t* rb = reinterpret_cast<volatile uint32_t*>(scratch_a);
        uint32_t expected = (r + 1) * 0x01010101u;
        bool ok = true;
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
            if (rb[w] != expected) { ok = false; break; }
        }
        results[1 + r] = ok ? 1 : 0;
        if (ok) correct++;
    }
    results[17] = NUM_ROWS_TEST;
    results[18] = correct;

    // ═══════════════════════════════════════════════════════════════════
    // SECTION B: Multi-bank-buffer saturation
    // ═══════════════════════════════════════════════════════════════════
    // Open K rows (each 8KB apart), then re-access the first row.
    // Time the re-access.  If K exceeds the number of row buffers,
    // the first row will have been evicted and we'll see a latency jump.

    uint64_t noc_base = get_noc_addr(dram_noc_x, dram_noc_y, base_addr);

    for (uint32_t K = 1; K <= 16; K++) {
        uint32_t median = 0;
        for (uint32_t trial = 0; trial < TIMING_TRIALS; trial++) {
            // Open base row
            noc_async_read(noc_base, scratch_a, CACHELINE);
            noc_async_read_barrier();

            // Open K-1 intervening rows to try to evict base from row buffer
            for (uint32_t j = 1; j < K; j++) {
                uint64_t noc_j = get_noc_addr(dram_noc_x, dram_noc_y,
                                               base_addr + j * ROW_SIZE);
                noc_async_read(noc_j, scratch_b, CACHELINE);
                noc_async_read_barrier();
            }

            // Time re-access of base row
            volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
            noc_async_read(noc_base, scratch_a, CACHELINE);
            noc_async_read_barrier();
            volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

            uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;
            uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;
            samples[trial] = static_cast<uint32_t>(t1 - t0);
        }
        // Insertion sort + median
        for (uint32_t i = 1; i < TIMING_TRIALS; i++) {
            uint32_t v = samples[i]; uint32_t j = i;
            while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
            samples[j] = v;
        }
        results[19 + (K - 1)] = samples[TIMING_TRIALS / 2];
    }

    // ═══════════════════════════════════════════════════════════════════
    // SECTION C: Large-stride sweep
    // ═══════════════════════════════════════════════════════════════════
    // Time (base, base + stride) for stride = 8KB, 16KB, 32KB, ..., 8MB
    // (16 power-of-2 steps).  If the real bank mapping uses higher address
    // bits or XOR hashing, we'll see a latency change at the true bank stride.

    for (uint32_t s = 0; s < 16; s++) {
        uint32_t stride = ROW_SIZE << s;  // 8KB, 16KB, 32KB, ..., 256MB
        // Cap at reasonable DRAM range
        if (stride > 8 * 1024 * 1024) {
            results[35 + s] = 0xFFFFFFFF;  // not tested
            continue;
        }

        uint64_t noc_far = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + stride);

        for (uint32_t trial = 0; trial < TIMING_TRIALS; trial++) {
            // Open base
            noc_async_read(noc_base, scratch_a, CACHELINE);
            noc_async_read_barrier();

            // Read far address to evict
            noc_async_read(noc_far, scratch_b, CACHELINE);
            noc_async_read_barrier();

            // Time re-access of base
            volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
            noc_async_read(noc_base, scratch_a, CACHELINE);
            noc_async_read_barrier();
            volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
            volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

            uint64_t t0 = (static_cast<uint64_t>(hi0) << 32) | lo0;
            uint64_t t1 = (static_cast<uint64_t>(hi1) << 32) | lo1;
            samples[trial] = static_cast<uint32_t>(t1 - t0);
        }
        for (uint32_t i = 1; i < TIMING_TRIALS; i++) {
            uint32_t v = samples[i]; uint32_t j = i;
            while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
            samples[j] = v;
        }
        results[35 + s] = samples[TIMING_TRIALS / 2];
    }

    // ═══════════════════════════════════════════════════════════════════
    // SECTION D: Write-read vs read-read
    // ═══════════════════════════════════════════════════════════════════
    // Compare: (read B, barrier, time read A) vs (write B, barrier, time read A)
    // Writes must commit to DRAM, so if the write path forces a real row
    // activation, the subsequent read of A might show different latency.

    uint64_t noc_a = get_noc_addr(dram_noc_x, dram_noc_y, base_addr);
    uint64_t noc_b = get_noc_addr(dram_noc_x, dram_noc_y, base_addr + ROW_SIZE);

    // D1: read-read
    for (uint32_t trial = 0; trial < TIMING_TRIALS; trial++) {
        noc_async_read(noc_b, scratch_b, CACHELINE);
        noc_async_read_barrier();

        volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        noc_async_read(noc_a, scratch_a, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

        samples[trial] = static_cast<uint32_t>(
            ((static_cast<uint64_t>(hi1) << 32) | lo1) -
            ((static_cast<uint64_t>(hi0) << 32) | lo0));
    }
    for (uint32_t i = 1; i < TIMING_TRIALS; i++) {
        uint32_t v = samples[i]; uint32_t j = i;
        while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
        samples[j] = v;
    }
    results[51] = samples[TIMING_TRIALS / 2];

    // D2: write-read (write to B, then time read of A)
    // Fill scratch_c with a pattern for writing
    volatile uint32_t* wfill = reinterpret_cast<volatile uint32_t*>(scratch_c);
    for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
        wfill[w] = 0xDEADBEEF;
    }

    for (uint32_t trial = 0; trial < TIMING_TRIALS; trial++) {
        noc_async_write(scratch_c, noc_b, CACHELINE);
        noc_async_write_barrier();

        volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
        noc_async_read(noc_a, scratch_a, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
        volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);

        samples[trial] = static_cast<uint32_t>(
            ((static_cast<uint64_t>(hi1) << 32) | lo1) -
            ((static_cast<uint64_t>(hi0) << 32) | lo0));
    }
    for (uint32_t i = 1; i < TIMING_TRIALS; i++) {
        uint32_t v = samples[i]; uint32_t j = i;
        while (j > 0 && samples[j-1] > v) { samples[j] = samples[j-1]; --j; }
        samples[j] = v;
    }
    results[52] = samples[TIMING_TRIALS / 2];
}
