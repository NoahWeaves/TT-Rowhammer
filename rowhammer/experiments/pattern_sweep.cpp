// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// C2 — Aggressor-data-pattern sweep host driver.  For each victim row in a
// configurable range, hammers it with every (victim_pattern, aggressor_pattern)
// combination from a small library and records the result to a CSV.
//
// Library of patterns (32-bit, repeated across the cache line):
//   solid0   = 0x00000000     all zeros
//   solid1   = 0xFFFFFFFF     all ones
//   stripe55 = 0x55555555     0101...
//   stripeAA = 0xAAAAAAAA     1010...
//   walking1 = 0x80808080     one '1' per byte
//   inv_walk = 0x7F7F7F7F     one '0' per byte
//   rand_fix = 0xC3F0A55A     fixed pseudo-random
//
// We sweep the *Cartesian product* victim × aggressor (49 combinations by
// default) at a small number of hammer iterations per pair so the whole
// matrix fits in a few minutes of wall time.

#include <array>
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

struct NamedPattern { const char* name; uint32_t value; };
static const NamedPattern PATTERNS[] = {
    {"solid0",   0x00000000u},
    {"solid1",   0xFFFFFFFFu},
    {"stripe55", 0x55555555u},
    {"stripeAA", 0xAAAAAAAAu},
    {"walking1", 0x80808080u},
    {"inv_walk", 0x7F7F7F7Fu},
    {"rand_fix", 0xC3F0A55Au},
};

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
static constexpr uint32_t SCRATCH_BYTES     = 2 * CACHELINE;
static constexpr uint32_t L1_ALIGN          = 64;
static constexpr double   NS_PER_CYCLE      = 1.25;

int main(int argc, char** argv) {
    uint32_t channel_idx       = 0;
    uint32_t start_row         = 64;
    uint32_t num_rows          = 8;
    uint32_t hammer_iterations = 500000;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--channel")    == 0 && i+1<argc) channel_idx       = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--start-row")  == 0 && i+1<argc) start_row         = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--num-rows")   == 0 && i+1<argc) num_rows          = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations") == 0 && i+1<argc) hammer_iterations = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help")       == 0) {
            fmt::print("Usage: {} [--channel N] [--start-row N] [--num-rows N] [--iterations N]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) { fmt::print(stderr, "bad channel\n"); return 1; }
    const DramChannel& ch = DRAM_CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];
    constexpr CoreCoord worker_core = {1, 2};

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + SCRATCH_BYTES + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t safe_row_min = (dram_base / ROW_SIZE) + 2;
    if (start_row < safe_row_min) start_row = safe_row_min;

    std::filesystem::path out_dir = "tt_metal/programming_examples/rowhammer/experiments/results";
    if (const char* home = std::getenv("TT_METAL_HOME")) {
        out_dir = std::filesystem::path(home) / out_dir;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    auto csv_path = out_dir / fmt::format("pattern_sweep_ch{}.csv", channel_idx);
    std::ofstream csv(csv_path);
    csv << "channel,row,victim_pattern,aggressor_pattern,activations,act_rate_M,num_flips,total_bit_flips\n";

    fmt::print("pattern_sweep  channel={} rows {}..{} iter={}  patterns={}x{}\n",
               ch.name, start_row, start_row+num_rows-1, hammer_iterations,
               sizeof(PATTERNS)/sizeof(PATTERNS[0]),
               sizeof(PATTERNS)/sizeof(PATTERNS[0]));
    fmt::print("CSV: {}\n\n", csv_path.string());

    uint32_t total_flips = 0;
    for (uint32_t r = 0; r < num_rows; ++r) {
        uint32_t victim_row  = start_row + r;
        uint32_t victim_addr = row_base(victim_row);
        for (const auto& vp : PATTERNS) for (const auto& ap : PATTERNS) {
            Program program = CreateProgram();
            KernelHandle kid = CreateKernel(
                program,
                OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_pattern_kernel.cpp",
                worker_core,
                DataMovementConfig{
                    .processor = DataMovementProcessor::RISCV_0,
                    .noc       = NOC::RISCV_0_default});
            SetRuntimeArgs(program, kid, worker_core, {
                ch.noc_x, ch.noc_y, victim_addr, hammer_iterations,
                vp.value, ap.value, scratch_addr, result_addr,
            });
            distributed::MeshWorkload workload;
            workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                                 std::move(program));
            distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
            distributed::Finish(cq);

            std::vector<uint32_t> result_vec;
            detail::ReadFromDeviceL1(device, worker_core, result_addr,
                                     RESULT_BUF_WORDS * sizeof(uint32_t), result_vec);
            uint32_t num_flips       = result_vec[1];
            uint32_t total_bit_flips = result_vec[2];
            uint32_t cycles_lo       = result_vec[3];
            uint32_t cycles_hi       = result_vec[4];
            uint32_t activations     = result_vec[5];
            uint64_t cycles          = (static_cast<uint64_t>(cycles_hi) << 32) | cycles_lo;
            double   act_rate_M      = (cycles > 0)
                                         ? activations / (cycles * NS_PER_CYCLE / 1e9) / 1e6
                                         : 0.0;
            total_flips += num_flips;

            if (num_flips > 0) {
                fmt::print("  ROW {} v={} a={}: {} FLIPS ({} bits, {:.1f}M act/s)\n",
                           victim_row, vp.name, ap.name, num_flips, total_bit_flips, act_rate_M);
            }
            csv << channel_idx << "," << victim_row << ",0x" << fmt::format("{:08x}", vp.value)
                << ",0x" << fmt::format("{:08x}", ap.value) << "," << activations << ","
                << act_rate_M << "," << num_flips << "," << total_bit_flips << "\n";
        }
    }
    fmt::print("\nDone — total flips across all (row, pattern) combinations: {}\n", total_flips);
    return 0;
}
