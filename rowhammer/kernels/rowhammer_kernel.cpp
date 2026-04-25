// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Rowhammer kernel for Tenstorrent Blackhole GDDR6.
//
// This kernel performs a double-sided rowhammer attack on a single DRAM channel.
// It writes a known data pattern to a victim row, then rapidly alternates reads
// between the two aggressor rows (victim ± 0x2000) in a tight pipelined loop
// to maximize the DRAM row activation rate.  After hammering, it reads the
// victim row back and compares against the expected pattern, reporting any
// bit flips found.
//
// === Address geometry (from TT-Rowhammer characterisation) ===
//   Row size:   8 KB  (8192 bytes, 128 × 64-byte cache lines)
//   Row select: bits [31:13]   → Row N = addr >> 13
//   Column:     bits [12:6]    → 128 columns per row
//   Byte:       bits [5:0]     → 64 bytes per NOC transaction
//   Adjacent rows are at ± 0x2000 (8 KB) offsets.
//   No XOR interleaving — sequential mapping confirmed on all 8 channels.
//
// === Design choices ===
//
// 1. PIPELINED READS (no barrier inside the hammer loop)
//    The characterisation showed that pipelined NOC reads achieve ~48-57
//    cycles/activation (14-17 M act/s) whereas serialised reads with a
//    barrier after each one cost ~436 cycles (~1.8 M act/s).  Removing the
//    barrier inside the tight loop is the single most important optimisation.
//    We only issue reads — we never need the returned data during hammering,
//    so the L1 destination can be overwritten freely.
//
// 2. ALTERNATING TWO AGGRESSOR ADDRESSES (double-sided)
//    Classic double-sided rowhammer alternates between the rows immediately
//    above and below the victim.  This causes the DRAM controller to
//    repeatedly open aggressor-low → close → open aggressor-high → close,
//    maximising the electrical disturbance on the victim row in between.
//
// 3. TWO-PHASE OPERATION (write pattern → hammer → verify)
//    Phase 1: Write a known pattern (0x55 or 0xAA) to every cache line of
//             the victim row using NOC async writes.
//    Phase 2: Hammer the two aggressors in a tight loop for a configurable
//             number of iterations.
//    Phase 3: Read back every cache line of the victim row and compare
//             against the expected pattern, recording any differences.
//
// 4. SINGLE DRAM CHANNEL, RAW NOC ADDRESSES
//    We bypass the interleaved buffer allocator entirely and construct raw
//    NOC addresses using get_noc_addr(noc_x, noc_y, dram_addr).  This lets
//    us target exact physical DRAM addresses on a specific channel, which is
//    necessary for rowhammer since the attack depends on physical adjacency.
//
// 5. WALL-CLOCK TIMING
//    We use the BRISC hardware wall clock (RISCV_DEBUG_REG_WALL_CLOCK_L/H)
//    to measure cycle-accurate duration of the hammering phase.  At 800 MHz,
//    1 cycle = 1.25 ns.
//
// 6. RESULTS VIA L1 MAILBOX
//    Results (flip count, flip offsets, timing) are written to a results
//    buffer in L1 whose address is passed as a runtime argument.  The host
//    reads this back after kernel completion.

#include <cstdint>

// ─── compile-time constants ───────────────────────────────────────────
// ROW_SIZE must be 8192 for Blackhole GDDR6.
constexpr uint32_t ROW_SIZE        = 8192;           // bytes per DRAM row
constexpr uint32_t CACHELINE       = 64;             // bytes per NOC txn
constexpr uint32_t CACHELINES_PER_ROW = ROW_SIZE / CACHELINE;  // 128

// ─── result buffer layout (written to L1, read by host) ───────────────
// Word 0:  status         (0 = ok, 1 = error)
// Word 1:  num_flips      (total cache lines with at least one bit flip)
// Word 2:  total_bit_flips (total individual bit flips across all lines)
// Word 3:  hammer_cycles  (lower 32 bits of wall-clock duration)
// Word 4:  hammer_cycles_hi (upper 32 bits)
// Word 5:  activations    (number of activation pairs issued)
// Words 6+: up to 32 flip descriptors, each 4 words:
//           [cacheline_index, word_offset, expected, actual]
constexpr uint32_t RESULT_HDR_WORDS  = 6;
constexpr uint32_t MAX_FLIP_RECORDS  = 32;
constexpr uint32_t FLIP_RECORD_WORDS = 4;
constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

void kernel_main() {
    // ─── runtime arguments (set by host) ──────────────────────────────
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);   // DRAM core NOC X
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);   // DRAM core NOC Y
    uint32_t victim_addr     = get_arg_val<uint32_t>(2);   // victim row base (must be 8KB aligned)
    uint32_t hammer_iters    = get_arg_val<uint32_t>(3);   // number of hammer iterations (each = 2 activations)
    uint32_t data_pattern    = get_arg_val<uint32_t>(4);   // 32-bit fill pattern for victim (e.g. 0x55555555)
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(5);   // L1 address for scratch (≥128 bytes)
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(6);   // L1 address for result buffer
    uint32_t use_barrier     = get_arg_val<uint32_t>(7);   // 0 = pipelined (fast), 1 = serialized (barrier each pair)

    // ─── derived addresses ────────────────────────────────────────────
    uint32_t aggressor_lo = victim_addr - ROW_SIZE;   // row N-1
    uint32_t aggressor_hi = victim_addr + ROW_SIZE;   // row N+1

    // L1 scratch: we use two 64-byte slots for the NOC read destinations.
    // The data is throwaway — we only read to trigger DRAM row activations.
    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;

    // Result buffer pointer
    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);

    // Zero result header
    for (uint32_t i = 0; i < RESULT_BUF_WORDS; i++) {
        results[i] = 0;
    }

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 1: Write known pattern to victim row
    // ═══════════════════════════════════════════════════════════════════
    // Fill the L1 scratch line with the data pattern, then write it to
    // every cache line of the victim row via NOC async writes.
    volatile uint32_t* fill = reinterpret_cast<volatile uint32_t*>(scratch_a);
    for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
        fill[w] = data_pattern;
    }

    for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
        uint64_t dst_noc = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
        noc_async_write(scratch_a, dst_noc, CACHELINE);
    }
    noc_async_write_barrier();   // ensure all writes land before hammering

    // Write COMPLEMENTARY pattern to aggressor rows.
    // This is critical: rowhammer relies on charge leakage from fully-charged
    // aggressor cells into depleted victim cells (or vice versa).  Writing the
    // same pattern to both would produce zero net disturbance.
    uint32_t aggressor_pattern = ~data_pattern;
    volatile uint32_t* fill_b = reinterpret_cast<volatile uint32_t*>(scratch_b);
    for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
        fill_b[w] = aggressor_pattern;
    }

    for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
        uint64_t lo_noc = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_lo + cl * CACHELINE);
        uint64_t hi_noc = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_hi + cl * CACHELINE);
        noc_async_write(scratch_b, lo_noc, CACHELINE);
        noc_async_write(scratch_b, hi_noc, CACHELINE);
    }
    noc_async_write_barrier();

    // Pre-compute the NOC addresses for the two aggressor rows (first cache line).
    // We hammer only the first cache line of each row — the DRAM controller must
    // activate the entire row for every read regardless of column.
    uint64_t noc_lo = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_lo);
    uint64_t noc_hi = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_hi);

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 2: Hammer loop — pipelined double-sided reads
    // ═══════════════════════════════════════════════════════════════════
    //
    // This is the performance-critical section.  We alternate between
    // reading from aggressor_lo and aggressor_hi with NO barrier between
    // reads.  The NOC read pipeline can have several requests in flight;
    // each read causes a DRAM row activation regardless of whether we
    // wait for the data.
    //
    // The destination L1 addresses (scratch_a/scratch_b) are recycled —
    // we don't care about the returned data, only about triggering
    // activations.

    // Read the wall clock before hammering
    volatile uint32_t timestamp_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t timestamp_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_start = (static_cast<uint64_t>(timestamp_hi) << 32) | timestamp_lo;

    if (use_barrier) {
        // Serialized mode: barrier after each read pair ensures actual DRAM row switch
        for (uint32_t i = 0; i < hammer_iters; i++) {
            noc_async_read(noc_lo, scratch_a, CACHELINE);
            noc_async_read_barrier();
            noc_async_read(noc_hi, scratch_b, CACHELINE);
            noc_async_read_barrier();
        }
    } else {
        // Pipelined mode: no barrier, maximum issue rate
        for (uint32_t i = 0; i < hammer_iters; i++) {
            noc_async_read(noc_lo, scratch_a, CACHELINE);
            noc_async_read(noc_hi, scratch_b, CACHELINE);
        }
    }

    // Drain the pipeline — wait for all outstanding reads to complete
    noc_async_read_barrier();

    // Read wall clock after hammering
    volatile uint32_t ts_end_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t ts_end_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_end = (static_cast<uint64_t>(ts_end_hi) << 32) | ts_end_lo;

    uint64_t elapsed = t_end - t_start;

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 3: Read back victim row and check for bit flips
    // ═══════════════════════════════════════════════════════════════════
    uint32_t num_flips = 0;
    uint32_t total_bit_flips = 0;
    uint32_t record_idx = 0;

    for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
        // Read one cache line from the victim row into scratch_a
        uint64_t victim_noc = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
        noc_async_read(victim_noc, scratch_a, CACHELINE);
        noc_async_read_barrier();

        // Compare every word against the expected pattern
        volatile uint32_t* readback = reinterpret_cast<volatile uint32_t*>(scratch_a);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
            uint32_t actual = readback[w];
            if (actual != data_pattern) {
                num_flips++;

                // Count individual bit flips
                uint32_t diff = actual ^ data_pattern;
                uint32_t bits = 0;
                while (diff) {
                    bits += diff & 1;
                    diff >>= 1;
                }
                total_bit_flips += bits;

                // Record the flip (up to MAX_FLIP_RECORDS)
                if (record_idx < MAX_FLIP_RECORDS) {
                    uint32_t base = RESULT_HDR_WORDS + record_idx * FLIP_RECORD_WORDS;
                    results[base + 0] = cl;              // cache line index (0-127)
                    results[base + 1] = w;               // word offset in line (0-15)
                    results[base + 2] = data_pattern;    // expected
                    results[base + 3] = actual;           // actual (with flipped bits)
                    record_idx++;
                }
            }
        }
    }

    // ─── write results ────────────────────────────────────────────────
    results[0] = 0;                                   // status (0 = completed)
    results[1] = num_flips;                           // words with flips
    results[2] = total_bit_flips;                     // individual bits flipped
    results[3] = static_cast<uint32_t>(elapsed);      // cycles (low 32)
    results[4] = static_cast<uint32_t>(elapsed >> 32);// cycles (high 32)
    results[5] = hammer_iters * 2;                    // total activations (2 per iter)
}
