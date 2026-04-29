// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Snapshot a remote tile's MMIO register via NOC into local L1.
//
// One-shot kernel: reads a single 4-byte register at (target_noc_x,
// target_noc_y, register_addr) and stores the value at l1_dst_addr.
// Used to read NIU_SLV_RD_REQ_RECEIVED on DRAM tiles for honest
// measurement of NOC reads delivered to the DRAM controller.
//
// Note: this read itself counts as +1 against the target's NIU_SLV
// counter — the host should subtract one snapshot read per probe.
//
// Runtime args:
//   arg 0:  target_noc_x
//   arg 1:  target_noc_y
//   arg 2:  register_addr
//   arg 3:  l1_dst_addr (>= 4 bytes)

#include <cstdint>

void kernel_main() {
    uint32_t tx       = get_arg_val<uint32_t>(0);
    uint32_t ty       = get_arg_val<uint32_t>(1);
    uint32_t reg_addr = get_arg_val<uint32_t>(2);
    uint32_t l1_dst   = get_arg_val<uint32_t>(3);

    uint64_t noc_addr = get_noc_addr(tx, ty, reg_addr);
    noc_async_read(noc_addr, l1_dst, 4);
    noc_async_read_barrier();
}
