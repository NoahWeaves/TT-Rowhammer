// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// B5 — Unit tests for the dram_addr_decoder against the explicit address
// pairs in TT-Rowhammer/row_verification.txt.  Exits 0 on full pass.
//
// The pairs are encoded inline so we don't depend on the TT-Rowhammer repo
// being on disk at run time.  They were extracted from row_verification.txt
// and represent every "explicit boundary pair" tested in characterization.

#include "dram_addr_decoder.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace rowhammer::dram;

namespace {

struct Pair {
    uint32_t a;
    uint32_t b;
    bool     same_row_expected;
};

// Pairs taken verbatim from TT-Rowhammer/row_verification.txt — every one
// was timed and classified same-row (833 cyc) vs. different-row (873 cyc).
constexpr Pair kPairs[] = {
    // Same-row pairs (median 833 cyc)
    {0,     64,     true},   // "Same row, 64B apart"
    {0,     4096,   true},   // "Same row, half-row apart"
    {0,     8128,   true},   // "Same row, edge (row_size-64)"
    {8192,  12288,  true},   // "Row 1 internal, half apart"
    {64,    4160,   true},   // "Same row, non-aligned"

    // Different-row pairs (median 873 cyc)
    {0,     8192,   false},  // "Different row, 1 row apart"
    {0,     8256,   false},  // "Different row, row+64B"
    {0,     16384,  false},  // "Different row, 2 rows apart"
    {0,     32768,  false},  // "Different row, 4 rows apart"
    {0,     65536,  false},  // "Different row, 8 rows apart"
    {8192,  16384,  false},  // "Row 1 vs Row 2"
    {64,    8256,   false},  // "Different row, non-aligned"
};

bool check_same_row() {
    bool ok = true;
    for (const Pair& p : kPairs) {
        bool got = same_row(p.a, p.b);
        if (got != p.same_row_expected) {
            std::fprintf(stderr,
                "FAIL same_row: a=0x%06x b=0x%06x expected %d got %d "
                "(row(a)=%u row(b)=%u)\n",
                p.a, p.b, p.same_row_expected, got, row_id(p.a), row_id(p.b));
            ok = false;
        }
    }
    return ok;
}

bool check_invariants() {
    bool ok = true;
    // Adjacent rows in both directions
    for (uint32_t row = 1; row < 64; ++row) {
        uint32_t v = row_base(row);
        if (row_id(aggressor_lo(v)) != row - 1) { std::fprintf(stderr, "FAIL aggressor_lo row=%u\n", row); ok = false; }
        if (row_id(aggressor_hi(v)) != row + 1) { std::fprintf(stderr, "FAIL aggressor_hi row=%u\n", row); ok = false; }
        if (byte_offset(v) != 0)                { std::fprintf(stderr, "FAIL byte_offset row=%u\n", row); ok = false; }
        if (column_id(v)  != 0)                 { std::fprintf(stderr, "FAIL column_id row=%u\n",  row); ok = false; }
    }
    // Bank-group boundary at bit 17 → ROWS_PER_BANK = 16
    if (ROWS_PER_BANK != 16)                                { std::fprintf(stderr, "FAIL ROWS_PER_BANK\n"); ok = false; }
    if (!same_bank_group(row_base(0),  row_base(15)))       { std::fprintf(stderr, "FAIL same_bank_group(0,15)\n"); ok = false; }
    if ( same_bank_group(row_base(0),  row_base(16)))       { std::fprintf(stderr, "FAIL bank_group cross at row 16\n"); ok = false; }
    if (column_id(0x000FC0) != 63)                          { std::fprintf(stderr, "FAIL column_id(0x000FC0)\n"); ok = false; }
    if (column_id(0x001FC0) != 127)                         { std::fprintf(stderr, "FAIL column_id(0x001FC0)\n"); ok = false; }
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= check_same_row();
    ok &= check_invariants();
    if (ok) {
        std::printf("test_addr_decoder: PASS (%zu pairs, plus invariants)\n",
                    sizeof(kPairs) / sizeof(kPairs[0]));
        return 0;
    }
    std::fprintf(stderr, "test_addr_decoder: FAIL\n");
    return 1;
}
