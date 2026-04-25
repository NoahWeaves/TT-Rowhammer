// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Multi-core / dual-RISC write hammer.  Implements two of the
// "actionable" next steps from the round-3 report:
//
//   B4: hammer from BRISC (RISCV_0/NOC_0) AND NCRISC (RISCV_1/NOC_1)
//       on the same core, doubling the injection paths.
//   B5/C7: spread the workload across many worker cores so the
//       aggregate activation rate is N× a single-core run, which
//       both stresses the controller harder and raises die
//       temperature (closer to the 70 °C+ regime where flips
//       have historically been observed).
//
// Each worker core gets its own victim row inside the same DRAM
// channel.  BRISC seeds and verifies its victim and counts flips;
// NCRISC pounds the same aggressors in parallel via the other NoC.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    {0,1,"ch0"}, {0,10,"ch1"}, {0,4,"ch2"}, {0,7,"ch3"},
    {9,1,"ch4"}, {9,10,"ch5"}, {9,4,"ch6"}, {9,7,"ch7"},
};

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
static constexpr uint32_t L1_ALIGN          = 64;
static constexpr double   NS_PER_CYCLE      = 1.25;

int main(int argc, char** argv) {
    uint32_t channel_idx     = 0;
    uint32_t hammer_iters    = 5'000'000;
    uint32_t num_aggressors  = 14;
    uint32_t stride_rows     = 1;
    uint32_t num_cores       = 8;
    uint32_t pattern         = 0x55555555;
    bool     dual_risc       = false;
    bool     all_channels    = false;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--channel")        == 0 && i+1<argc) channel_idx     = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations")     == 0 && i+1<argc) hammer_iters    = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--num-aggressors") == 0 && i+1<argc) num_aggressors  = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--stride")         == 0 && i+1<argc) stride_rows     = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--num-cores")      == 0 && i+1<argc) num_cores       = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--pattern")        == 0 && i+1<argc) pattern         = std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--dual-risc")      == 0)             dual_risc       = true;
        else if (std::strcmp(argv[i], "--all-channels")   == 0)             all_channels    = true;
        else if (std::strcmp(argv[i], "--help")           == 0) {
            fmt::print("Usage: {} [--channel N] [--iterations N] [--num-aggressors N (1..30)]\n"
                       "       [--stride N] [--num-cores N] [--pattern 0xNN]\n"
                       "       [--dual-risc] [--all-channels]\n", argv[0]);
            return 0;
        }
    }
    if (num_aggressors < 1)  num_aggressors = 1;
    if (num_aggressors > 30) num_aggressors = 30;
    if (stride_rows == 0)    stride_rows = 1;
    if (num_cores < 1)       num_cores = 1;

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];

    auto grid = mesh_device->compute_with_storage_grid_size();
    uint32_t grid_total = grid.x * grid.y;
    if (num_cores > grid_total) num_cores = grid_total;

    std::vector<CoreCoord> cores;
    cores.reserve(num_cores);
    for (uint32_t c = 0; c < num_cores; ++c) {
        cores.push_back(CoreCoord{c % grid.x, c / grid.x});
    }
    CoreRange core_range(cores.front(), cores.back());
    // We may have a non-rectangular set if num_cores doesn't fill a row;
    // build an exact CoreRangeSet of the requested cores.
    std::set<CoreRange> ranges;
    for (auto& c : cores) ranges.insert(CoreRange(c, c));
    CoreRangeSet core_set(ranges);

    uint32_t l1_base       = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr  = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t aux_scratch   = scratch_addr + 2 * 64;
    uint32_t result_addr   = (aux_scratch + 64 + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t safe_row_min = (dram_base / ROW_SIZE) + 2 + num_aggressors * stride_rows;

    uint32_t first_channel = all_channels ? 0 : channel_idx;
    uint32_t last_channel  = all_channels ? 7 : channel_idx;

    uint64_t grand_acts = 0, grand_flips = 0;

    for (uint32_t ch_i = first_channel; ch_i <= last_channel; ++ch_i) {
        const DramChannel& ch = DRAM_CHANNELS[ch_i];
        fmt::print("\n── ch{} ({},{}) cores={} dual_risc={} aggr={} stride={} iters={} pat=0x{:08x} ──\n",
                   ch_i, ch.noc_x, ch.noc_y, num_cores, dual_risc ? "yes" : "no",
                   num_aggressors, stride_rows, hammer_iters, pattern);

        Program program = CreateProgram();

        KernelHandle kid_brisc = CreateKernel(
            program,
            OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_write_kernel.cpp",
            core_set,
            DataMovementConfig{
                .processor = DataMovementProcessor::RISCV_0,
                .noc       = NOC::RISCV_0_default});

        KernelHandle kid_ncrisc = 0;
        if (dual_risc) {
            kid_ncrisc = CreateKernel(
                program,
                OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_write_aux_kernel.cpp",
                core_set,
                DataMovementConfig{
                    .processor = DataMovementProcessor::RISCV_1,
                    .noc       = NOC::RISCV_1_default});
        }

        // Per-core: pick a unique victim row well separated from other cores.
        uint32_t row_stride_per_core = (2 * num_aggressors * stride_rows + 1) * 4;
        std::vector<uint32_t> victim_rows(num_cores);
        for (uint32_t c = 0; c < num_cores; ++c) {
            victim_rows[c] = safe_row_min + 256 + c * row_stride_per_core;
        }

        for (uint32_t c = 0; c < num_cores; ++c) {
            uint32_t victim_row  = victim_rows[c];
            uint32_t victim_addr = row_base(victim_row);

            std::vector<uint32_t> aggrs;
            aggrs.reserve(num_aggressors);
            for (uint32_t k = 1; aggrs.size() < num_aggressors; ++k) {
                if (victim_row >= k * stride_rows + safe_row_min)
                    aggrs.push_back(row_base(victim_row - k * stride_rows));
                if (aggrs.size() < num_aggressors)
                    aggrs.push_back(row_base(victim_row + k * stride_rows));
            }

            std::vector<uint32_t> brisc_args = {
                ch.noc_x, ch.noc_y, victim_addr, hammer_iters, pattern,
                scratch_addr, result_addr, num_aggressors,
            };
            for (uint32_t a : aggrs) brisc_args.push_back(a);
            SetRuntimeArgs(program, kid_brisc, cores[c], brisc_args);

            if (dual_risc) {
                std::vector<uint32_t> ncrisc_args = {
                    ch.noc_x, ch.noc_y, hammer_iters, aux_scratch, ~pattern, num_aggressors,
                };
                for (uint32_t a : aggrs) ncrisc_args.push_back(a);
                SetRuntimeArgs(program, kid_ncrisc, cores[c], ncrisc_args);
            }
        }

        distributed::MeshWorkload workload;
        workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                             std::move(program));
        distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
        distributed::Finish(cq);

        // Per-core readback.
        uint64_t ch_acts = 0, ch_flips = 0;
        double max_rate = 0.0;
        for (uint32_t c = 0; c < num_cores; ++c) {
            std::vector<uint32_t> rv;
            detail::ReadFromDeviceL1(device, cores[c], result_addr,
                                     RESULT_BUF_WORDS * sizeof(uint32_t), rv);
            uint32_t flips        = rv[1];
            uint32_t bit_flips    = rv[2];
            uint64_t cycles       = (static_cast<uint64_t>(rv[4]) << 32) | rv[3];
            uint32_t activations  = rv[5];
            double   rate_M       = (cycles > 0)
                                      ? activations / (cycles * NS_PER_CYCLE / 1e9) / 1e6
                                      : 0.0;
            if (rate_M > max_rate) max_rate = rate_M;
            ch_acts  += activations;
            ch_flips += bit_flips;
            if (flips > 0 || c < 2 || c == num_cores - 1) {
                fmt::print("   core ({:>2},{:>2}) row {:>5}: flips={} bit_flips={} acts={} {:.2f} M act/s\n",
                           cores[c].x, cores[c].y, victim_rows[c], flips, bit_flips, activations, rate_M);
            }
        }
        // Dual-risc multiplies activations: NCRISC issued the same iters*aggr writes.
        uint64_t physical_acts = dual_risc ? ch_acts * 2 : ch_acts;
        fmt::print("   ── ch{} totals: brisc-counted_acts={} (physical≈{}) bit_flips={} peak={:.2f} M act/s/core\n",
                   ch_i, ch_acts, physical_acts, ch_flips, max_rate);
        grand_acts  += physical_acts;
        grand_flips += ch_flips;
    }

    fmt::print("\n═══════════════════════════════════════════════\n");
    fmt::print("  MULTI-CORE WRITE HAMMER SUMMARY\n");
    fmt::print("  Total physical activations: {}\n", grand_acts);
    fmt::print("  Total visible bit flips:    {}\n", grand_flips);
    fmt::print("═══════════════════════════════════════════════\n");
    return 0;
}
