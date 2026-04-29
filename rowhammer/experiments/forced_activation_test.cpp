// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Forced-activation test — compares three hammering strategies to measure
// REAL activation rates and test whether confirmed activations produce flips.
//
//   Mode 0: FLUSH-READ   — 128MB flush before each read (confirmed real)
//   Mode 1: WRITE        — write to aggressors (must commit)
//   Mode 2: WRITE+READ   — write then read back (full cycle)
//   Mode 3: PIPELINED    — standard pipelined reads (baseline, may coalesce)
//
// Runs each mode on the same victim row and reports rate + flip count.

#include <algorithm>
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

static constexpr uint32_t ROW_SIZE         = 8192;
static constexpr uint32_t CACHELINE        = 64;
static constexpr double   NS_PER_CYCLE     = 1.25;
static constexpr uint32_t ROWS_PER_BANK    = 16;

static constexpr uint32_t RESULT_HDR_WORDS   = 6;
static constexpr uint32_t MAX_FLIP_RECORDS   = 32;
static constexpr uint32_t FLIP_RECORD_WORDS  = 4;
static constexpr uint32_t RESULT_BUF_WORDS   = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
static constexpr uint32_t L1_ALIGN = 64;

struct DramChannel { uint32_t noc_x; uint32_t noc_y; const char* name; };
static const DramChannel DRAM_CHANNELS[] = {
    {0,1,"ch0 (0,1)"},{0,10,"ch1 (0,10)"},{0,4,"ch2 (0,4)"},{0,7,"ch3 (0,7)"},
    {9,1,"ch4 (9,1)"},{9,10,"ch5 (9,10)"},{9,4,"ch6 (9,4)"},{9,7,"ch7 (9,7)"},
};

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

struct ModeConfig {
    uint32_t mode;
    const char* name;
    const char* kernel;
    uint32_t iters;  // adjusted per mode for comparable wall time
};

int main(int argc, char** argv) {
    uint32_t channel_idx  = 0;
    uint32_t start_row    = 2000;
    uint32_t num_sides    = 2;    // double-sided for clearest signal
    uint32_t base_iters   = 500000;
    uint32_t data_pattern = 0x55555555;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--channel") == 0 && i+1 < argc)
            channel_idx = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--start-row") == 0 && i+1 < argc)
            start_row = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--num-sides") == 0 && i+1 < argc)
            num_sides = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations") == 0 && i+1 < argc)
            base_iters = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [--channel N] [--start-row N] [--num-sides N] [--iterations N]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) return 1;
    const DramChannel& ch = DRAM_CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];
    constexpr CoreCoord worker_core = {1, 2};

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_size  = RESULT_BUF_WORDS * sizeof(uint32_t);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (start_row < min_safe_row) start_row = min_safe_row;

    // Center in bank
    uint32_t bank_start = (start_row / ROWS_PER_BANK) * ROWS_PER_BANK;
    start_row = bank_start + ROWS_PER_BANK / 2;
    uint32_t victim_addr = start_row * ROW_SIZE;

    // Build aggressor list (closest rows first)
    std::vector<uint32_t> aggr_addrs;
    for (uint32_t dist = 1; aggr_addrs.size() < num_sides && dist < ROWS_PER_BANK; dist++) {
        if (start_row >= dist + bank_start)
            aggr_addrs.push_back((start_row - dist) * ROW_SIZE);
        if (aggr_addrs.size() < num_sides && start_row + dist <= bank_start + ROWS_PER_BANK - 1)
            aggr_addrs.push_back((start_row + dist) * ROW_SIZE);
    }

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Forced-Activation Comparison Test                     ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:     {} ({},{})\n", ch.name, ch.noc_x, ch.noc_y);
    fmt::print("║  Victim:      row {} (0x{:08x})\n", start_row, victim_addr);
    fmt::print("║  Aggressors:  {} (rows:", aggr_addrs.size());
    for (auto a : aggr_addrs) fmt::print(" {}", a / ROW_SIZE);
    fmt::print(")\n");
    fmt::print("║  Pattern:     0x{:08x}\n", data_pattern);
    fmt::print("║  Base iters:  {}\n", base_iters);
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    // Modes to test (flush modes are slower, so fewer iters for comparable wall time)
    std::vector<ModeConfig> modes = {
        {0, "FLUSH-READ (confirmed real)", "rowhammer/kernels/forced_activation_kernel.cpp", base_iters / 10},
        {3, "FLUSH-WRITE (confirmed real)","rowhammer/kernels/forced_activation_kernel.cpp", base_iters / 10},
        {1, "WRITE pipelined (no barrier)","rowhammer/kernels/forced_activation_kernel.cpp", base_iters},
        {4, "WRITE serialized (barrier)",  "rowhammer/kernels/forced_activation_kernel.cpp", base_iters / 2},
        {2, "WRITE+READ (full cycle)",     "rowhammer/kernels/forced_activation_kernel.cpp", base_iters / 5},
        {5, "PIPELINED READ (baseline)",   "rowhammer/kernels/rowhammer_nsided_kernel.cpp",  base_iters},
    };

    fmt::print("{:<35s} | {:>10s} | {:>10s} | {:>10s} | {:>5s} | {:>5s}\n",
               "Mode", "Acts", "Time (ms)", "M act/s", "Flips", "ECC?");
    fmt::print("{:-<35s}-+-{:-<10s}-+-{:-<10s}-+-{:-<10s}-+-{:-<5s}-+-{:-<5s}\n",
               "", "", "", "", "", "");

    for (auto& mc : modes) {
        Program program = CreateProgram();

        if (mc.mode <= 4) {
            // Forced-activation kernel
            KernelHandle kid = CreateKernel(
                program,
                std::string(OVERRIDE_KERNEL_PREFIX) + mc.kernel,
                worker_core,
                DataMovementConfig{
                    .processor = DataMovementProcessor::RISCV_0,
                    .noc       = NOC::RISCV_0_default});

            std::vector<uint32_t> args = {
                ch.noc_x, ch.noc_y, victim_addr, mc.iters, data_pattern,
                scratch_addr, result_addr, mc.mode,
                static_cast<uint32_t>(aggr_addrs.size()),
            };
            for (auto a : aggr_addrs) args.push_back(a);
            SetRuntimeArgs(program, kid, worker_core, args);
        } else {
            // Standard n-sided kernel (for baseline comparison)
            KernelHandle kid = CreateKernel(
                program,
                std::string(OVERRIDE_KERNEL_PREFIX) + mc.kernel,
                worker_core,
                DataMovementConfig{
                    .processor = DataMovementProcessor::RISCV_0,
                    .noc       = NOC::RISCV_0_default});

            std::vector<uint32_t> args = {
                ch.noc_x, ch.noc_y, victim_addr, mc.iters, data_pattern,
                scratch_addr, result_addr,
                0u,  // use_barrier
                static_cast<uint32_t>(aggr_addrs.size()),
                0u,  // core_id
                0u,  // delay_iters
            };
            for (auto a : aggr_addrs) args.push_back(a);
            SetRuntimeArgs(program, kid, worker_core, args);
        }

        distributed::MeshWorkload workload;
        workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                             std::move(program));
        distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
        distributed::Finish(cq);

        std::vector<uint32_t> rv;
        detail::ReadFromDeviceL1(device, worker_core, result_addr, result_size, rv);

        uint32_t num_flips   = rv[1];
        uint64_t cycles      = (static_cast<uint64_t>(rv[4]) << 32) | rv[3];
        uint32_t activations = rv[5];
        double   elapsed_ms  = cycles * NS_PER_CYCLE / 1e6;
        double   act_rate_M  = (cycles > 0)
            ? activations / (cycles * NS_PER_CYCLE / 1e9) / 1e6
            : 0.0;

        fmt::print("{:<35s} | {:>10d} | {:>10.1f} | {:>10.2f} | {:>5d} | {:>5s}\n",
                   mc.name, activations, elapsed_ms, act_rate_M,
                   num_flips, num_flips > 0 ? "---" : "no");

        if (num_flips > 0) {
            uint32_t records = std::min(num_flips, MAX_FLIP_RECORDS);
            for (uint32_t r = 0; r < records; r++) {
                uint32_t base = RESULT_HDR_WORDS + r * FLIP_RECORD_WORDS;
                fmt::print("  Flip #{}: CL {} word {}: exp 0x{:08x} got 0x{:08x}\n",
                           r+1, rv[base], rv[base+1], rv[base+2], rv[base+3]);
            }
        }
    }

    fmt::print("\nInterpretation:\n");
    fmt::print("  FLUSH-READ / FLUSH-WRITE = confirmed real activation rate\n");
    fmt::print("  WRITE pipelined / FLUSH-WRITE = write coalescing factor\n");
    fmt::print("  WRITE serialized = does barrier alone prevent coalescing?\n");
    fmt::print("  PIPELINED READ / FLUSH-READ = read coalescing factor (~29x)\n");

    mesh_device->close();
    return 0;
}
