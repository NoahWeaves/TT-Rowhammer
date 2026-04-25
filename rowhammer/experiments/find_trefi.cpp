// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// C1 — find_trefi.  Sweeps the per-loop delay of the REF-sync hammer kernel
// and prints achieved activation rate vs. delay so the operator can identify
// the synchronization plateau that mirrors GPUHammer Fig. 5.
//
// Output: stdout table, plus a CSV at
//   tt-metal/tt_metal/programming_examples/rowhammer/experiments/results/
//   find_trefi_<channel>_<row>.csv
// Columns: delay_iters, activations, elapsed_us, act_rate_M_per_s, num_flips
//
// Pass criterion (recorded by operator): a flat plateau of >=14 M act/s
// across a contiguous range of delay values, *and* (optionally) a cluster
// of victim flips inside that plateau.

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

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
static constexpr uint32_t SCRATCH_BYTES     = 2 * CACHELINE;
static constexpr uint32_t L1_ALIGN          = 64;
static constexpr double   NS_PER_CYCLE      = 1.25;

int main(int argc, char** argv) {
    uint32_t channel_idx       = 0;
    uint32_t hammer_iterations = 200000;       // shorter than full attack — calibration only
    uint32_t row_offset        = 128;          // victim row above safe baseline
    uint32_t delay_min         = 0;
    uint32_t delay_max         = 256;
    uint32_t delay_step        = 8;
    uint32_t data_pattern      = 0x55555555;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--channel")    == 0 && i+1<argc) channel_idx       = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations") == 0 && i+1<argc) hammer_iterations = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--row")        == 0 && i+1<argc) row_offset        = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--delay-min")  == 0 && i+1<argc) delay_min         = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--delay-max")  == 0 && i+1<argc) delay_max         = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--delay-step") == 0 && i+1<argc) delay_step        = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--pattern")    == 0 && i+1<argc) data_pattern      = std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--help")       == 0) {
            fmt::print("Usage: {} [--channel N] [--iterations N] [--row N]"
                       " [--delay-min N] [--delay-max N] [--delay-step N]"
                       " [--pattern 0xNN]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8 || delay_step == 0 || delay_max < delay_min) {
        fmt::print(stderr, "bad args\n");
        return 1;
    }
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
    uint32_t victim_row   = std::max(row_offset, safe_row_min + 1);
    uint32_t victim_addr  = row_base(victim_row);

    fmt::print("find_trefi  channel={} ({},{})  victim_row={}  iter/step={}\n",
               ch.name, ch.noc_x, ch.noc_y, victim_row, hammer_iterations);

    // Set up CSV output
    std::filesystem::path out_dir = "tt_metal/programming_examples/rowhammer/experiments/results";
    if (const char* home = std::getenv("TT_METAL_HOME")) {
        out_dir = std::filesystem::path(home) / out_dir;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    std::filesystem::path csv_path =
        out_dir / fmt::format("find_trefi_ch{}_row{}.csv", channel_idx, victim_row);
    std::ofstream csv(csv_path);
    csv << "delay_iters,activations,elapsed_us,act_rate_M_per_s,num_flips,total_bit_flips\n";

    fmt::print("\ndelay |  activations  | elapsed (μs) | M act/s | flips\n");
    fmt::print("------+---------------+--------------+---------+------\n");

    double best_rate = 0.0;
    uint32_t best_delay = 0;

    for (uint32_t delay = delay_min; delay <= delay_max; delay += delay_step) {
        Program program = CreateProgram();
        KernelHandle kid = CreateKernel(
            program,
            OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_refsync_kernel.cpp",
            worker_core,
            DataMovementConfig{
                .processor = DataMovementProcessor::RISCV_0,
                .noc       = NOC::RISCV_0_default});

        SetRuntimeArgs(program, kid, worker_core, {
            ch.noc_x, ch.noc_y, victim_addr, hammer_iterations, data_pattern,
            scratch_addr, result_addr, delay,
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
        double   elapsed_us      = cycles * NS_PER_CYCLE / 1000.0;
        double   act_rate_M      = (cycles > 0)
                                     ? activations / (cycles * NS_PER_CYCLE / 1e9) / 1e6
                                     : 0.0;

        if (act_rate_M > best_rate) { best_rate = act_rate_M; best_delay = delay; }

        fmt::print("{:5d} | {:13d} | {:12.1f} | {:7.2f} | {}\n",
                   delay, activations, elapsed_us, act_rate_M, num_flips);
        csv << delay << "," << activations << "," << elapsed_us << ","
            << act_rate_M << "," << num_flips << "," << total_bit_flips << "\n";
    }

    fmt::print("\nbest rate: {:.2f} M act/s @ delay={}\n", best_rate, best_delay);
    fmt::print("CSV written: {}\n", csv_path.string());
    fmt::print("Look for a flat plateau ≥14 M act/s in the CSV — that is the\n");
    fmt::print("REF-aligned synchronization band (cf. GPUHammer Fig. 5).\n");
    return 0;
}
