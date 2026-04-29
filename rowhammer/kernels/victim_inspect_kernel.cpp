// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Victim-row inspector kernel.
//
// Reads `num_cachelines` cachelines starting at `dram_base_addr` into a
// contiguous L1 buffer at `l1_dst_addr`. Used by peak_attack.cpp to
// snapshot the victim row before and after the hammer, so the host can
// hash/diff/dump the bytes.
//
// === Runtime arguments ===
//   arg 0:  dram_noc_x
//   arg 1:  dram_noc_y
//   arg 2:  dram_base_addr
//   arg 3:  num_cachelines
//   arg 4:  l1_dst_addr

#include <cstdint>

constexpr uint32_t CACHELINE = 64;

void kernel_main() {
    uint32_t dram_noc_x     = get_arg_val<uint32_t>(0);
    uint32_t dram_noc_y     = get_arg_val<uint32_t>(1);
    uint32_t dram_base_addr = get_arg_val<uint32_t>(2);
    uint32_t num_cachelines = get_arg_val<uint32_t>(3);
    uint32_t l1_dst_addr    = get_arg_val<uint32_t>(4);

    for (uint32_t cl = 0; cl < num_cachelines; cl++) {
        uint64_t src = get_noc_addr(dram_noc_x, dram_noc_y,
                                    dram_base_addr + cl * CACHELINE);
        noc_async_read(src, l1_dst_addr + cl * CACHELINE, CACHELINE);
    }
    noc_async_read_barrier();
}
