// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Core-scaling coalescing sweep.
//
// Hammers a single victim row (and its same-bank aggressors) from N cores
// in parallel using the FLUSH-READ kernel, where every read is a confirmed
// real DRAM activation (128 MB flush forces row buffer closed). Sweeps N
// across {1, 2, 4, 8, 16, 32, 64, 128} (or a custom set) and reports per-core
// and aggregate real activation rates, plus the implied coalescing factor
// relative to the single-core baseline.
//
// Hypothesis under test: if the GDDR6 controller's reorder buffer is the
// limiting factor, per-core real act/s should stay flat as N grows and
// aggregate rate should scale linearly. If the per-bank physical activation
// rate is the limit, per-core rate should collapse as 1/N and aggregate
// should plateau.

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

static constexpr uint32_t ROW_SIZE        = 8192;
static constexpr uint32_t CACHELINE       = 64;
static constexpr double   NS_PER_CYCLE    = 1.25;
static constexpr uint32_t ROWS_PER_BANK   = 16;

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
static constexpr uint32_t L1_ALIGN          = 64;

struct DramChannel { uint32_t noc_x; uint32_t noc_y; const char* name; };
static const DramChannel DRAM_CHANNELS[] = {
    {0,1,"ch0 (0,1)"},{0,10,"ch1 (0,10)"},{0,4,"ch2 (0,4)"},{0,7,"ch3 (0,7)"},
    {9,1,"ch4 (9,1)"},{9,10,"ch5 (9,10)"},{9,4,"ch6 (9,4)"},{9,7,"ch7 (9,7)"},
};

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

// Enumerate the device's compute-with-storage grid in logical coordinates.
// This already excludes dispatch cores and harvested rows, so we can safely
// use any (x,y) it returns. Row-major fill order.
static std::vector<CoreCoord> compute_grid_workers(distributed::MeshDevice* mesh) {
    CoreCoord g = mesh->compute_with_storage_grid_size();
    std::vector<CoreCoord> out;
    out.reserve(g.x * g.y);
    for (uint32_t y = 0; y < g.y; y++) {
        for (uint32_t x = 0; x < g.x; x++) out.push_back({x, y});
    }
    return out;
}

int main(int argc, char** argv) {
    uint32_t channel_idx  = 0;
    uint32_t start_row    = 2000;
    uint32_t num_sides    = 8;       // same-bank aggressors per core
    uint32_t hammer_iters = 50000;   // FLUSH-READ is slow; 50k×8 ≈ 0.4 s/core
    uint32_t data_pattern = 0x55555555;
    std::vector<uint32_t> sweep    = {1, 2, 4, 8, 16, 32, 64, 128};

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--channel") == 0 && i+1 < argc)
            channel_idx = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--start-row") == 0 && i+1 < argc)
            start_row = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--num-sides") == 0 && i+1 < argc)
            num_sides = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations") == 0 && i+1 < argc)
            hammer_iters = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--sweep") == 0 && i+1 < argc) {
            sweep.clear();
            const char* s = argv[++i];
            while (*s) {
                char* end;
                uint32_t v = std::strtoul(s, &end, 10);
                if (end == s) break;
                sweep.push_back(v);
                s = end;
                if (*s == ',') s++;
            }
        }
        else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [--channel N] [--start-row N] [--num-sides N]\n"
                       "              [--iterations N] [--sweep N1,N2,...]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) { fmt::print(stderr, "channel out of range\n"); return 1; }
    const DramChannel& ch = DRAM_CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];

    auto workers = compute_grid_workers(mesh_device.get());
    uint32_t max_workers = workers.size();
    for (auto& n : sweep) if (n > max_workers) n = max_workers;
    // de-dup after capping
    std::vector<uint32_t> dedup;
    for (auto n : sweep) if (dedup.empty() || dedup.back() != n) dedup.push_back(n);
    sweep = std::move(dedup);

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_size  = RESULT_BUF_WORDS * sizeof(uint32_t);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (start_row < min_safe_row) start_row = min_safe_row;

    // Center victim within its bank
    uint32_t bank_start = (start_row / ROWS_PER_BANK) * ROWS_PER_BANK;
    start_row = bank_start + ROWS_PER_BANK / 2;
    uint32_t victim_addr = start_row * ROW_SIZE;

    // Same-bank aggressors, closest first, alternating sides
    std::vector<uint32_t> aggr_addrs;
    for (uint32_t dist = 1; aggr_addrs.size() < num_sides && dist < ROWS_PER_BANK; dist++) {
        if (start_row >= dist + bank_start)
            aggr_addrs.push_back((start_row - dist) * ROW_SIZE);
        if (aggr_addrs.size() < num_sides && start_row + dist <= bank_start + ROWS_PER_BANK - 1)
            aggr_addrs.push_back((start_row + dist) * ROW_SIZE);
    }

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Core-Scaling Coalescing Sweep (FLUSH-READ)            ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:        {} ({},{})\n", ch.name, ch.noc_x, ch.noc_y);
    fmt::print("║  Victim row:     {} (0x{:08x})\n", start_row, victim_addr);
    fmt::print("║  Aggressors:     {} (rows:", aggr_addrs.size());
    for (auto a : aggr_addrs) fmt::print(" {}", a / ROW_SIZE);
    fmt::print(")\n");
    fmt::print("║  Iterations:     {} per core (× {} aggressors = {} acts/core)\n",
               hammer_iters, aggr_addrs.size(), hammer_iters * aggr_addrs.size());
    fmt::print("║  Worker pool:    {} cores available\n", max_workers);
    fmt::print("║  Sweep:         ");
    for (auto n : sweep) fmt::print(" {}", n);
    fmt::print("\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    fmt::print("{:>6s} | {:>14s} | {:>12s} | {:>12s} | {:>14s} | {:>10s} | {:>5s}\n",
               "cores", "total acts", "max ms", "M act/s/core", "M act/s aggr",
               "vs 1-core", "flips");
    fmt::print("{:->6s}-+-{:->14s}-+-{:->12s}-+-{:->12s}-+-{:->14s}-+-{:->10s}-+-{:->5s}\n",
               "", "", "", "", "", "", "");

    double baseline_per_core = 0.0;

    for (uint32_t N : sweep) {
        std::vector<CoreCoord> sel(workers.begin(), workers.begin() + N);

        // CoreRangeSet covering all selected cores (one CoreRange per (x) span
        // is overkill; one CoreRange per core is correct and simple).
        std::set<CoreRange> ranges;
        for (auto& c : sel) ranges.insert(CoreRange(c, c));
        CoreRangeSet core_set(ranges);

        Program program = CreateProgram();
        KernelHandle kid = CreateKernel(
            program,
            std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/core_scaling_kernel.cpp",
            core_set,
            DataMovementConfig{
                .processor = DataMovementProcessor::RISCV_0,
                .noc       = NOC::RISCV_0_default});

        for (uint32_t c = 0; c < N; c++) {
            std::vector<uint32_t> args = {
                ch.noc_x, ch.noc_y, victim_addr, hammer_iters, data_pattern,
                scratch_addr, result_addr,
                c,                                          // core_id
                static_cast<uint32_t>(aggr_addrs.size()),
            };
            for (auto a : aggr_addrs) args.push_back(a);
            SetRuntimeArgs(program, kid, sel[c], args);
        }

        distributed::MeshWorkload workload;
        workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                             std::move(program));
        distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
        distributed::Finish(cq);

        // Aggregate per-core results
        uint64_t total_acts = 0;
        uint64_t max_cycles = 0;
        double   sum_per_core_rate = 0.0;
        uint32_t total_flips = 0;
        for (uint32_t c = 0; c < N; c++) {
            std::vector<uint32_t> rv;
            detail::ReadFromDeviceL1(device, sel[c], result_addr, result_size, rv);
            uint32_t acts   = rv[5];
            uint64_t cycles = (static_cast<uint64_t>(rv[4]) << 32) | rv[3];
            total_acts += acts;
            if (cycles > max_cycles) max_cycles = cycles;
            if (cycles > 0) sum_per_core_rate += acts / (cycles * NS_PER_CYCLE / 1e9);
            if (c == 0) total_flips = rv[1];
        }
        double avg_per_core_M = (sum_per_core_rate / N) / 1e6;
        double aggr_M         = (max_cycles > 0)
            ? total_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6
            : 0.0;
        double max_ms         = max_cycles * NS_PER_CYCLE / 1e6;

        if (N == sweep.front()) baseline_per_core = avg_per_core_M;
        double ratio = (baseline_per_core > 0) ? avg_per_core_M / baseline_per_core : 1.0;

        fmt::print("{:>6d} | {:>14d} | {:>12.1f} | {:>12.3f} | {:>14.3f} | {:>9.2f}x | {:>5d}\n",
                   N, static_cast<int>(total_acts), max_ms,
                   avg_per_core_M, aggr_M, ratio, total_flips);
    }

    fmt::print("\nInterpretation:\n");
    fmt::print("  per-core rate flat as N grows  -> NOC/per-core bound; scaling helps linearly.\n");
    fmt::print("  per-core rate falls ~1/N       -> per-bank controller saturation; scaling does NOT help.\n");
    fmt::print("  aggregate rate plateau         -> upper bound on real act/s reachable on this bank.\n");
    fmt::print("  flips > 0                      -> the architectural mitigation has been broken.\n");

    mesh_device->close();
    return 0;
}
