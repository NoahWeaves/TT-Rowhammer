// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Sub-port aliasing probe.
//
// Writes a unique 32-bit pattern at `victim_addr` via the FIRST sub-port,
// then reads back via all THREE sub-ports of the same channel and copies
// the first word of each read into L1 result slots [0..2].
//
// If all three reads return the same pattern → sub-ports alias to the same
// physical address. If only sub-port 0 reads the pattern → sub-ports map to
// distinct memory regions.
//
// === Runtime args ===
//   arg 0:  sp0_noc_x        (write target)
//   arg 1:  sp0_noc_y
//   arg 2:  sp1_noc_x
//   arg 3:  sp1_noc_y
//   arg 4:  sp2_noc_x
//   arg 5:  sp2_noc_y
//   arg 6:  victim_addr
//   arg 7:  pattern
//   arg 8:  l1_scratch_addr  (≥ 4 cachelines)
//   arg 9:  l1_result_addr   (3 uint32 slots)

#include <cstdint>

constexpr uint32_t CACHELINE = 64;

void kernel_main() {
    uint32_t sp0x = get_arg_val<uint32_t>(0);
    uint32_t sp0y = get_arg_val<uint32_t>(1);
    uint32_t sp1x = get_arg_val<uint32_t>(2);
    uint32_t sp1y = get_arg_val<uint32_t>(3);
    uint32_t sp2x = get_arg_val<uint32_t>(4);
    uint32_t sp2y = get_arg_val<uint32_t>(5);
    uint32_t victim_addr = get_arg_val<uint32_t>(6);
    uint32_t pattern = get_arg_val<uint32_t>(7);
    uint32_t scratch = get_arg_val<uint32_t>(8);
    uint32_t result  = get_arg_val<uint32_t>(9);

    uint32_t write_buf = scratch;
    uint32_t r0 = scratch + CACHELINE;
    uint32_t r1 = scratch + 2 * CACHELINE;
    uint32_t r2 = scratch + 3 * CACHELINE;

    // Fill write buffer with the pattern
    volatile uint32_t* w = reinterpret_cast<volatile uint32_t*>(write_buf);
    for (uint32_t i = 0; i < CACHELINE / sizeof(uint32_t); i++) w[i] = pattern;

    // Write via sub-port 0
    uint64_t dst = get_noc_addr(sp0x, sp0y, victim_addr);
    noc_async_write(write_buf, dst, CACHELINE);
    noc_async_write_barrier();

    // Read via all three sub-ports
    noc_async_read(get_noc_addr(sp0x, sp0y, victim_addr), r0, CACHELINE);
    noc_async_read_barrier();
    noc_async_read(get_noc_addr(sp1x, sp1y, victim_addr), r1, CACHELINE);
    noc_async_read_barrier();
    noc_async_read(get_noc_addr(sp2x, sp2y, victim_addr), r2, CACHELINE);
    noc_async_read_barrier();

    volatile uint32_t* res = reinterpret_cast<volatile uint32_t*>(result);
    res[0] = *reinterpret_cast<volatile uint32_t*>(r0);
    res[1] = *reinterpret_cast<volatile uint32_t*>(r1);
    res[2] = *reinterpret_cast<volatile uint32_t*>(r2);
}
