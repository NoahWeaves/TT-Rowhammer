// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Header-only DRAM address decoder for Tenstorrent Blackhole GDDR6.
//
// Encodes the address layout characterized in TT-Rowhammer:
//
//   Byte address bits:  [31:13]   [12:6]   [5:0]
//                        ROW       COLUMN   BYTE
//
// Same-bank-group test uses the cross-bank-group bit boundary observed in
// the address-bit-toggle measurements (873 vs 897 cycle median): bits 13..16
// toggle stays inside a bank (873 cyc), bits 17+ jump to a different bank
// group (897 cyc).  ROWS_PER_BANK = 16 is therefore inferred to be 1 << 4 =
// 16; this constant is the SUBJECT of verification step B2 and may be
// updated based on measured data.

#pragma once

#include <cstdint>

namespace rowhammer::dram {

// ─── geometry constants ──────────────────────────────────────────────
inline constexpr uint32_t ROW_SIZE              = 8192;        // 8 KB
inline constexpr uint32_t CACHELINE             = 64;          // bytes/NOC txn
inline constexpr uint32_t CACHELINES_PER_ROW    = ROW_SIZE / CACHELINE;  // 128
inline constexpr uint32_t COLUMNS_PER_ROW       = CACHELINES_PER_ROW;
inline constexpr uint32_t ROW_SHIFT             = 13;          // log2(8192)
inline constexpr uint32_t COL_SHIFT             = 6;           // log2(64)
inline constexpr uint32_t BANK_GROUP_SHIFT      = 17;          // bits 17+ cross bank groups (inferred)
inline constexpr uint32_t ROWS_PER_BANK         = 1u << (BANK_GROUP_SHIFT - ROW_SHIFT);  // 16

// ─── decoders ────────────────────────────────────────────────────────
constexpr uint32_t row_id(uint32_t addr)         { return addr >> ROW_SHIFT; }
constexpr uint32_t column_id(uint32_t addr)      { return (addr >> COL_SHIFT) & (COLUMNS_PER_ROW - 1); }
constexpr uint32_t byte_offset(uint32_t addr)    { return addr & (CACHELINE - 1); }
constexpr uint32_t bank_group_id(uint32_t addr)  { return addr >> BANK_GROUP_SHIFT; }

constexpr uint32_t row_base(uint32_t row)        { return row << ROW_SHIFT; }
constexpr uint32_t aggressor_lo(uint32_t victim) { return victim - ROW_SIZE; }
constexpr uint32_t aggressor_hi(uint32_t victim) { return victim + ROW_SIZE; }

constexpr bool same_row(uint32_t a, uint32_t b)        { return row_id(a) == row_id(b); }
constexpr bool same_bank_group(uint32_t a, uint32_t b) { return bank_group_id(a) == bank_group_id(b); }

}  // namespace rowhammer::dram
