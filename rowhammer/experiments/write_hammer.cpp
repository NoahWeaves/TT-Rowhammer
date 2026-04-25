// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Host driver for write-based n-sided rowhammer (technique C6).
//
// Builds an aggressor set around the victim row using a configurable stride
// and aggressor count, then runs the write-hammer kernel.  The default
// stride matches the assumed ROWS_PER_BANK=16 same-bank model
// (consecutive rows around the victim within one bank).  --stride and
// --num-aggressors let you sweep alternative geometries when the assumed
// model is unverified.

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
    {0,1,"ch0 (0,1)"}, {0,10,"ch1 (0,10)"}, {0,4,"ch2 (0,4)"}, {0,7,"ch3 (0,7)"},
    {9,1,"ch4 (9,1)"}, {9,10,"ch5 (9,10)"}, {9,4,"ch6 (9,4)"}, {9,7,"ch7 (9,7)"},
};

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
static constexpr uint32_t SCRATCH_BYTES     = 2 * 64;
static constexpr uint32_t L1_ALIGN          = 64;
static constexpr double   NS_PER_CYCLE      = 1.25;

int main(int argc, char** argv) {
    uint32_t channel_idx     = 0;
    uint32_t victim_row_arg  = 0;        // 0 ⇒ auto-pick first safe row
    uint32_t num_rows        = 4;
    uint32_t hammer_iters    = 5'000'000;
    uint32_t num_aggressors  = 14;       // up to 30
    uint32_t stride_rows     = 1;        // 1 = consecutive rows; >1 explores
                                          // alternative geometries
    uint32_t pattern         = 0x55555555;
    bool     all_channels    = false;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--channel")        == 0 && i+1<argc) channel_idx     = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--start-row")      == 0 && i+1<argc) victim_row_arg  = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--num-rows")       == 0 && i+1<argc) num_rows        = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations")     == 0 && i+1<argc) hammer_iters    = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--num-aggressors") == 0 && i+1<argc) num_aggressors  = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--stride")         == 0 && i+1<argc) stride_rows     = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--pattern")        == 0 && i+1<argc) pattern         = std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--all-channels")   == 0)             all_channels    = true;
        else if (std::strcmp(argv[i], "--help")           == 0) {
            fmt::print("Usage: {} [--channel N] [--start-row N] [--num-rows N]\n"
                       "                 [--iterations N] [--num-aggressors N (1-30)]\n"
                       "                 [--stride N] [--pattern 0xNN] [--all-channels]\n",
                       argv[0]);
            return 0;
        }
    }
    if (num_aggressors < 1)  num_aggressors = 1;
    if (num_aggressors > 30) num_aggressors = 30;
    if (stride_rows == 0)    stride_rows = 1;

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];
    constexpr CoreCoord worker_core = {1, 2};

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + SCRATCH_BYTES + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t safe_row_min = (dram_base / ROW_SIZE) + 2 + num_aggressors * stride_rows;

    uint32_t first_channel = all_channels ? 0 : channel_idx;
    uint32_t last_channel  = all_channels ? 7 : channel_idx;

    uint64_t grand_acts  = 0;
    uint64_t grand_flips = 0;

    for (uint32_t ch_i = first_channel; ch_i <= last_channel; ++ch_i) {
        const DramChannel& ch = DRAM_CHANNELS[ch_i];
        fmt::print("\n── ch{} ({},{}) ───── iters={} aggr={} stride={} pat=0x{:08x} ──────\n",
                   ch_i, ch.noc_x, ch.noc_y, hammer_iters, num_aggressors, stride_rows, pattern);

        uint32_t base_victim_row = (victim_row_arg ? victim_row_arg : safe_row_min);
        if (base_victim_row < safe_row_min) base_victim_row = safe_row_min;

        for (uint32_t r = 0; r < num_rows; ++r) {
            uint32_t victim_row  = base_victim_row + r * (2 * num_aggressors * stride_rows + 1);
            uint32_t victim_addr = row_base(victim_row);

            // Build aggressor set: alternate above and below the victim, stride_rows apart.
            std::vector<uint32_t> aggr_addrs;
            aggr_addrs.reserve(num_aggressors);
            for (uint32_t k = 1; aggr_addrs.size() < num_aggressors; ++k) {
                uint32_t below = victim_row - k * stride_rows;
                uint32_t above = victim_row + k * stride_rows;
                if (below >= safe_row_min)
                    aggr_addrs.push_back(row_base(below));
                if (aggr_addrs.size() < num_aggressors)
                    aggr_addrs.push_back(row_base(above));
            }

            Program program = CreateProgram();
            KernelHandle kid = CreateKernel(
                program,
                OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_write_kernel.cpp",
                worker_core,
                DataMovementConfig{
                    .processor = DataMovementProcessor::RISCV_0,
                    .noc       = NOC::RISCV_0_default});

            std::vector<uint32_t> args = {
                ch.noc_x, ch.noc_y, victim_addr, hammer_iters, pattern,
                scratch_addr, result_addr, num_aggressors,
            };
            for (uint32_t a : aggr_addrs) args.push_back(a);
            SetRuntimeArgs(program, kid, worker_core, args);

            distributed::MeshWorkload workload;
            workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                                 std::move(program));
            distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
            distributed::Finish(cq);

            std::vector<uint32_t> rv;
            detail::ReadFromDeviceL1(device, worker_core, result_addr,
                                     RESULT_BUF_WORDS * sizeof(uint32_t), rv);
            uint32_t flips        = rv[1];
            uint32_t bit_flips    = rv[2];
            uint64_t cycles       = (static_cast<uint64_t>(rv[4]) << 32) | rv[3];
            uint32_t activations  = rv[5];
            double   elapsed_us   = cycles * NS_PER_CYCLE / 1000.0;
            double   rate_M       = (cycles > 0)
                                      ? activations / (cycles * NS_PER_CYCLE / 1e9) / 1e6
                                      : 0.0;

            fmt::print("   row {:>5} (0x{:x}): flips={}  bit_flips={}  acts={}  {:.2f} M act/s\n",
                       victim_row, victim_addr, flips, bit_flips, activations, rate_M);

            grand_acts  += activations;
            grand_flips += bit_flips;
            (void)elapsed_us;
        }
    }

    fmt::print("\n═══════════════════════════════════════════════\n");
    fmt::print("  WRITE HAMMER SUMMARY\n");
    fmt::print("  Total activations:       {}\n", grand_acts);
    fmt::print("  Total bit flips visible: {}\n", grand_flips);
    fmt::print("═══════════════════════════════════════════════\n");

    return 0;
}
