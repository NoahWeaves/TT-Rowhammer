// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// C3 — DRAMA-style row-set discovery.  Empirically classify which DRAM row
// addresses share a bank with a chosen anchor row, instead of *assuming* the
// `victim ± k * 0x2000` model.  Mirrors GPUHammer Step 1.
//
// Procedure:
//   For an anchor address A and a sweep range of K rows, time the access
//   pair (A, A + k * 8 KB) for k in 1..K using the geometry_probe_kernel.
//   Classify each k by latency tier:
//     ≤ 850 cyc  → same row (only happens at k = 0 in a row sweep, but we
//                  keep the bucket so noise can be visualized)
//     851–890   → SAME-BANK, different row  (the row-buffer-conflict set)
//     891+      → cross-bank-group
//
// Output: a CSV plus a same-bank "row set" file in
//   experiments/results/row_set_ch<C>_anchor<R>.txt — one offset per line,
//   suitable for feeding back into the n-sided hammer driver as the
//   measured aggressor list.
//
// Validation against the assumed model: report what fraction of the
// discovered same-bank slots agree with the predicted set
// {A + k * 0x2000 : k % ROWS_PER_BANK ∈ same_bank}.  Disagreement is itself
// the high-value finding — the assumed geometry would be wrong.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/core.h>

#include <tt-metalium/host_api.hpp>
#include <tt-metalium/tt_metal.hpp>
#include <tt-metalium/device.hpp>
#include <tt-metalium/distributed.hpp>
#include <tt-metalium/allocator.hpp>
#include <tt-metalium/hal_types.hpp>

#include "experiments/dram_addr_decoder.hpp"

using namespace tt;
using namespace tt::tt_metal;
using namespace rowhammer::dram;

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

struct DramChannel { uint32_t noc_x; uint32_t noc_y; const char* name; };
static const DramChannel DRAM_CHANNELS[] = {
    {0,1,"ch0 (0,1)"},{0,10,"ch1 (0,10)"},{0,4,"ch2 (0,4)"},{0,7,"ch3 (0,7)"},
    {9,1,"ch4 (9,1)"},{9,10,"ch5 (9,10)"},{9,4,"ch6 (9,4)"},{9,7,"ch7 (9,7)"},
};

static constexpr uint32_t MAX_PROBES_PER_BATCH = 64;
static constexpr uint32_t RESULT_WORDS         = 1 + MAX_PROBES_PER_BATCH;
static constexpr uint32_t SCRATCH_BYTES        = 2 * 1024;  // matches geometry_probe_kernel EVICT_BURST
static constexpr uint32_t L1_ALIGN             = 64;

// Latency tier thresholds — must stay aligned with verify_geometry.cpp.
static constexpr uint32_t SAME_ROW_MAX     = 850;
static constexpr uint32_t SAME_BANK_MAX    = 890;

static std::vector<uint32_t> probe_batch(
    distributed::MeshDevice& md,
    distributed::MeshCommandQueue& cq,
    IDevice* device,
    CoreCoord worker_core,
    uint32_t scratch_addr,
    uint32_t result_addr,
    const DramChannel& ch,
    const std::vector<std::pair<uint32_t,uint32_t>>& probes)
{
    std::vector<uint32_t> all;
    all.reserve(probes.size());
    for (size_t base = 0; base < probes.size(); base += MAX_PROBES_PER_BATCH) {
        size_t batch = std::min<size_t>(MAX_PROBES_PER_BATCH, probes.size() - base);
        Program program = CreateProgram();
        KernelHandle kid = CreateKernel(
            program,
            OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/geometry_probe_kernel.cpp",
            worker_core,
            DataMovementConfig{
                .processor = DataMovementProcessor::RISCV_0,
                .noc       = NOC::RISCV_0_default});
        std::vector<uint32_t> args;
        args.reserve(5 + 2 * batch);
        args.push_back(ch.noc_x);
        args.push_back(ch.noc_y);
        args.push_back(scratch_addr);
        args.push_back(result_addr);
        args.push_back(static_cast<uint32_t>(batch));
        for (size_t i = 0; i < batch; ++i) {
            args.push_back(probes[base + i].first);
            args.push_back(probes[base + i].second);
        }
        SetRuntimeArgs(program, kid, worker_core, args);
        distributed::MeshWorkload workload;
        workload.add_program(distributed::MeshCoordinateRange(md.shape()), std::move(program));
        distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
        distributed::Finish(cq);
        std::vector<uint32_t> result_vec;
        detail::ReadFromDeviceL1(device, worker_core, result_addr,
                                 RESULT_WORDS * sizeof(uint32_t), result_vec);
        for (size_t i = 0; i < batch; ++i) all.push_back(result_vec[1 + i]);
    }
    return all;
}

int main(int argc, char** argv) {
    uint32_t channel_idx = 0;
    uint32_t anchor_row  = 256;       // anchor offset above safe baseline
    uint32_t sweep_rows  = 256;       // K rows above anchor to test (~ 2 MB)

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--channel") == 0 && i+1<argc) channel_idx = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--anchor")  == 0 && i+1<argc) anchor_row  = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rows")    == 0 && i+1<argc) sweep_rows  = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help")    == 0) {
            fmt::print("Usage: {} [--channel N] [--anchor row] [--rows K]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) return 1;
    const DramChannel& ch = DRAM_CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto md = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = md->mesh_command_queue();
    IDevice* device = md->get_devices()[0];
    constexpr CoreCoord worker_core = {1, 2};

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + SCRATCH_BYTES + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t safe_row_min = (dram_base / ROW_SIZE) + 4;
    if (anchor_row < safe_row_min) anchor_row = safe_row_min;
    uint32_t anchor_addr  = row_base(anchor_row);

    // Build sweep probes: (anchor, anchor + k*8KB) for k = 1..sweep_rows.
    std::vector<std::pair<uint32_t,uint32_t>> probes;
    probes.reserve(sweep_rows);
    for (uint32_t k = 1; k <= sweep_rows; ++k) {
        probes.emplace_back(anchor_addr, row_base(anchor_row + k));
    }

    fmt::print("build_row_set  channel={} anchor_row={} sweep={} rows\n",
               ch.name, anchor_row, sweep_rows);

    auto cycles = probe_batch(*md, cq, device, worker_core, scratch_addr, result_addr, ch, probes);

    // Classify and emit.
    std::filesystem::path out_dir = "tt_metal/programming_examples/rowhammer/experiments/results";
    if (const char* home = std::getenv("TT_METAL_HOME")) {
        out_dir = std::filesystem::path(home) / out_dir;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    auto csv_path     = out_dir / fmt::format("row_set_ch{}_anchor{}.csv",     channel_idx, anchor_row);
    auto rowset_path  = out_dir / fmt::format("row_set_ch{}_anchor{}.txt",     channel_idx, anchor_row);

    std::ofstream csv(csv_path);
    std::ofstream rowset(rowset_path);
    csv << "k,offset_bytes,row,cycles,tier\n";
    rowset << "# Empirically discovered same-bank rows for channel " << ch.name
           << " anchor row " << anchor_row << " (addr 0x" << std::hex << anchor_addr << std::dec << ")\n";
    rowset << "# tier = SAME_BANK (851..890 cyc); offsets in bytes from channel base\n";

    uint32_t same_bank = 0, cross_bank = 0, predicted_match = 0, predicted_total = 0;
    for (uint32_t k = 1; k <= sweep_rows; ++k) {
        uint32_t cyc = cycles[k - 1];
        uint32_t off = row_base(anchor_row + k);
        const char* tier;
        if      (cyc <= SAME_ROW_MAX)  tier = "SAME_ROW";
        else if (cyc <= SAME_BANK_MAX) { tier = "SAME_BANK"; ++same_bank; rowset << off << "\n"; }
        else                            { tier = "DIFF_BANK_GRP"; ++cross_bank; }
        csv << k << "," << off << "," << (anchor_row + k) << "," << cyc << "," << tier << "\n";

        // Compare against the assumed model: rows in the same 16-row aligned
        // bank as the anchor.
        uint32_t predicted_same_bank = (((anchor_row + k) / ROWS_PER_BANK) ==
                                        (anchor_row / ROWS_PER_BANK));
        if (predicted_same_bank) {
            ++predicted_total;
            if (std::strcmp(tier, "SAME_BANK") == 0) ++predicted_match;
        }
    }

    fmt::print("\nSame-bank rows discovered: {}\n", same_bank);
    fmt::print("Cross-bank-group rows:     {}\n",  cross_bank);
    if (predicted_total > 0) {
        double agreement = 100.0 * predicted_match / predicted_total;
        fmt::print("Agreement with assumed ROWS_PER_BANK={} model: {}/{} = {:.1f}%\n",
                   ROWS_PER_BANK, predicted_match, predicted_total, agreement);
        if (agreement < 90.0) {
            fmt::print("  ⚠️ <90%% — assumed geometry may be wrong, investigate before "
                       "running n-sided campaigns.\n");
        }
    }
    fmt::print("CSV:    {}\nRow-set:{}\n", csv_path.string(), rowset_path.string());
    return 0;
}
