// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// NCRISC auxiliary write hammer.  Runs alongside the BRISC write kernel
// on the same worker core but uses RISCV_1 / NOC_1, providing an
// independent injection path to the GDDR controller.  No seed, no
// verify — pure hammer loop.  Argument layout matches the BRISC
// kernel from index 0..3 so we can share runtime args layout where
// convenient, but this kernel is self-contained.
//
// Args:
//   0:  dram_noc_x
//   1:  dram_noc_y
//   2:  hammer_iters
//   3:  l1_scratch_addr     one cacheline used as the write source
//   4:  aggressor_pattern   uint32 written to aggressor cells
//   5:  num_aggressors      1..30
//   6+: aggressor DRAM byte offsets

#include <cstdint>

constexpr uint32_t CACHELINE      = 64;
constexpr uint32_t MAX_AGGRESSORS = 30;

void kernel_main() {
    uint32_t dram_noc_x        = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y        = get_arg_val<uint32_t>(1);
    uint32_t hammer_iters      = get_arg_val<uint32_t>(2);
    uint32_t scratch_addr      = get_arg_val<uint32_t>(3);
    uint32_t aggressor_pattern = get_arg_val<uint32_t>(4);
    uint32_t num_aggressors    = get_arg_val<uint32_t>(5);
    if (num_aggressors > MAX_AGGRESSORS) num_aggressors = MAX_AGGRESSORS;

    {
        volatile uint32_t* fp = reinterpret_cast<volatile uint32_t*>(scratch_addr);
        for (uint32_t w = 0; w < CACHELINE / 4; ++w) fp[w] = aggressor_pattern;
    }

    uint64_t aggr_noc[MAX_AGGRESSORS];
    for (uint32_t i = 0; i < num_aggressors; ++i) {
        uint32_t off = get_arg_val<uint32_t>(6 + i);
        aggr_noc[i]  = get_noc_addr(dram_noc_x, dram_noc_y, off);
    }

    for (uint32_t k = 0; k < hammer_iters; ++k) {
        for (uint32_t a = 0; a < num_aggressors; ++a) {
            noc_async_write(scratch_addr, aggr_noc[a], CACHELINE);
        }
    }
    noc_async_write_barrier();
}
