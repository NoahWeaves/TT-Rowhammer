// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Bank conflict probe — finds the true same-bank stride using pipelined
// burst timing with 128MB flush (same technique as validation_probe.cpp).
//
// Sweeps strides from 1×8KB to 64×8KB and reports per-read cycle cost.
// Same-bank conflicts show ~873 cyc, different-bank shows ~833 cyc.
// The periodicity reveals the real bank interleaving pattern.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fmt/core.h>

#include <tt-metalium/host_api.hpp>
#include <tt-metalium/tt_metal.hpp>
#include <tt-metalium/device.hpp>
#include <tt-metalium/distributed.hpp>
#include <tt-metalium/allocator.hpp>
#include <tt-metalium/hal_types.hpp>

using namespace tt;
using namespace tt::tt_metal;

static constexpr uint32_t ROW_SIZE     = 8192;
static constexpr uint32_t L1_ALIGN     = 64;
static constexpr uint32_t MAX_STRIDES  = 64;
static constexpr uint32_t RESULT_WORDS = 1 + MAX_STRIDES;

struct DramChannel { uint32_t noc_x; uint32_t noc_y; const char* name; };
static const DramChannel DRAM_CHANNELS[] = {
    {0,1,"ch0 (0,1)"},{0,10,"ch1 (0,10)"},{0,4,"ch2 (0,4)"},{0,7,"ch3 (0,7)"},
    {9,1,"ch4 (9,1)"},{9,10,"ch5 (9,10)"},{9,4,"ch6 (9,4)"},{9,7,"ch7 (9,7)"},
};

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

int main(int argc, char** argv) {
    uint32_t channel_idx = 0;
    uint32_t max_stride_rows = 64;  // test strides 1..64 rows (8KB..512KB)

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--channel") == 0 && i+1 < argc)
            channel_idx = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--max-stride") == 0 && i+1 < argc)
            max_stride_rows = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [--channel N] [--max-stride N]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) return 1;
    if (max_stride_rows > MAX_STRIDES) max_stride_rows = MAX_STRIDES;
    const DramChannel& ch = DRAM_CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];
    constexpr CoreCoord worker_core = {1, 2};

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + 256 + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    // Anchor well above allocator base, with headroom for +128MB flush
    uint32_t safe_row     = (dram_base / ROW_SIZE) + 128;
    uint32_t anchor_addr  = safe_row * ROW_SIZE;

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Bank Conflict Probe (True Same-Bank Stride)           ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:      {} ({},{})\n", ch.name, ch.noc_x, ch.noc_y);
    fmt::print("║  Anchor:       row {} (0x{:08x})\n", safe_row, anchor_addr);
    fmt::print("║  Stride range: 1..{} rows ({}KB..{}KB)\n",
               max_stride_rows, 8, max_stride_rows * 8);
    fmt::print("║  Technique:    pipelined 8-pair burst + 128MB flush\n");
    fmt::print("║                (same as validation_probe.cpp)\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    // Build stride list
    std::vector<uint32_t> strides;
    for (uint32_t s = 1; s <= max_stride_rows; s++) {
        strides.push_back(s * ROW_SIZE);
    }

    Program program = CreateProgram();
    KernelHandle kid = CreateKernel(
        program,
        OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/bank_conflict_probe_kernel.cpp",
        worker_core,
        DataMovementConfig{
            .processor = DataMovementProcessor::RISCV_0,
            .noc       = NOC::RISCV_0_default});

    std::vector<uint32_t> args;
    args.reserve(6 + strides.size());
    args.push_back(ch.noc_x);
    args.push_back(ch.noc_y);
    args.push_back(scratch_addr);
    args.push_back(result_addr);
    args.push_back(anchor_addr);
    args.push_back(static_cast<uint32_t>(strides.size()));
    for (auto s : strides) args.push_back(s);
    SetRuntimeArgs(program, kid, worker_core, args);

    distributed::MeshWorkload workload;
    workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                         std::move(program));
    distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
    distributed::Finish(cq);

    std::vector<uint32_t> rv;
    detail::ReadFromDeviceL1(device, worker_core, result_addr,
                             RESULT_WORDS * sizeof(uint32_t), rv);

    // Classify based on raw total cycles for 8 pairs (16 reads).
    // From validation_probe: 833 cyc/read for same-row, 873 for same-bank conflict.
    // With 8 pairs = 16 reads:
    //   same-row total ≈ 833 * 16 ≈ 13328
    //   same-bank-conflict total ≈ 873 * 16 ≈ 13968 (but MORE row switches → even higher)
    //   diff-bank total ≈ 833 * 16 ≈ 13328 (parallel banks, no conflict)
    // The KEY signal: same-bank pairs have HIGHER total than diff-bank pairs.
    // We look for the ~640 cycle delta (873-833 = 40 cyc/read × 16 reads).
    //
    // But since pipelined reads overlap, the actual total is much lower.
    // Let's just report raw values and look for any variation.

    fmt::print("stride  | rows |    KB | total cyc (8 pairs) | per-read | note\n");
    fmt::print("--------+------+-------+---------------------+----------+-----\n");

    // Find min/max for dynamic thresholding
    uint32_t min_cyc = UINT32_MAX, max_cyc = 0;
    for (uint32_t i = 0; i < strides.size(); i++) {
        uint32_t cyc = rv[1 + i];
        if (cyc < min_cyc) min_cyc = cyc;
        if (cyc > max_cyc) max_cyc = cyc;
    }
    uint32_t range = max_cyc - min_cyc;
    // If range > 30 cycles, there's meaningful variation
    uint32_t high_threshold = min_cyc + (range > 30 ? range / 2 : 9999);

    uint32_t conflict_strides = 0;
    uint32_t first_conflict = 0;
    uint32_t conflict_period = 0;
    uint32_t prev_conflict = 0;

    for (uint32_t i = 0; i < strides.size(); i++) {
        uint32_t stride_rows = i + 1;
        uint32_t stride_kb   = stride_rows * 8;
        uint32_t cyc         = rv[1 + i];
        uint32_t per_read    = cyc / 16;

        const char* note = "";
        if (range > 30 && cyc > high_threshold) {
            note = "*** HIGH (bank conflict?) ***";
            conflict_strides++;
            if (first_conflict == 0) {
                first_conflict = stride_rows;
            } else if (conflict_period == 0 && prev_conflict > 0) {
                conflict_period = stride_rows - prev_conflict;
            }
            prev_conflict = stride_rows;
        }

        fmt::print("{:7d} | {:4d} | {:5d} | {:19d} | {:8d} | {}\n",
                   strides[i], stride_rows, stride_kb, cyc, per_read, note);
    }

    fmt::print("\n  Raw cycle range: {} to {} (delta={})\n", min_cyc, max_cyc, range);

    fmt::print("\n═══ ANALYSIS ═════════════════════════════════════════════════\n");
    fmt::print("  Same-bank conflicts found: {}/{}\n", conflict_strides, strides.size());
    if (first_conflict > 0) {
        fmt::print("  First conflict at stride:  {} rows ({}KB)\n",
                   first_conflict, first_conflict * 8);
    }
    if (conflict_period > 0) {
        fmt::print("  Conflict period:           every {} rows ({}KB)\n",
                   conflict_period, conflict_period * 8);
        fmt::print("\n  → True same-bank stride is likely {} rows ({}KB)\n",
                   conflict_period, conflict_period * 8);
        fmt::print("  → Aggressors should be at victim ± {}KB, not ± 8KB!\n",
                   conflict_period * 8);
    } else if (first_conflict > 0) {
        fmt::print("  Only one conflict found — need wider sweep to find period\n");
    } else {
        fmt::print("  No same-bank conflicts found in sweep range.\n");
        fmt::print("  The assumed ROWS_PER_BANK model may need a wider stride.\n");
    }

    // Print a compact summary line for easy reference
    fmt::print("\n  Latency spectrum: ");
    for (uint32_t i = 0; i < strides.size(); i++) {
        uint32_t cyc = rv[1 + i];
        if (range > 30 && cyc > high_threshold) fmt::print("X");
        else fmt::print(".");
    }
    fmt::print("\n  Legend: . = baseline, X = elevated (possible bank conflict)\n");

    mesh_device->close();
    return 0;
}
