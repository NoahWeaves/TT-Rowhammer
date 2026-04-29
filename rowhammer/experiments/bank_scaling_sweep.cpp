// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Cross-bank scaling sweep.
//
// Companion to core_scaling_sweep. The single-bank sweep showed per-bank
// real activation rate plateaus at ~24 M act/s past ~16 cores. This sweep
// answers: do independent banks parallelize cleanly, or is there a shared
// upstream bottleneck (controller front-end, NOC, PCIe, etc.)?
//
// Design: pin `cores_per_bank` cores (default 16, the saturation knee) to
// each of B "banks" and sweep B across {1, 2, 4, 8}. Each bank is a separate
// GDDR6 channel with its own victim row and same-bank aggressor set, so the
// banks are physically independent (separate dies).
//
// Reuses core_scaling_kernel.cpp — each core just gets different runtime
// args pointing at its assigned channel/victim.

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
    {0,1,"ch0"},{0,10,"ch1"},{0,4,"ch2"},{0,7,"ch3"},
    {9,1,"ch4"},{9,10,"ch5"},{9,4,"ch6"},{9,7,"ch7"},
};
static constexpr uint32_t NUM_CHANNELS = 8;

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

static std::vector<CoreCoord> compute_grid_workers(distributed::MeshDevice* mesh) {
    CoreCoord g = mesh->compute_with_storage_grid_size();
    std::vector<CoreCoord> out;
    out.reserve(g.x * g.y);
    for (uint32_t y = 0; y < g.y; y++) {
        for (uint32_t x = 0; x < g.x; x++) out.push_back({x, y});
    }
    return out;
}

struct BankTarget {
    uint32_t channel_idx;
    uint32_t victim_row;
    uint32_t victim_addr;
    std::vector<uint32_t> aggr_addrs;
};

static BankTarget make_target(uint32_t channel_idx, uint32_t start_row,
                              uint32_t num_sides, uint32_t min_safe_row) {
    if (start_row < min_safe_row) start_row = min_safe_row;
    uint32_t bank_start = (start_row / ROWS_PER_BANK) * ROWS_PER_BANK;
    uint32_t victim_row = bank_start + ROWS_PER_BANK / 2;
    BankTarget t;
    t.channel_idx = channel_idx;
    t.victim_row  = victim_row;
    t.victim_addr = victim_row * ROW_SIZE;
    for (uint32_t dist = 1; t.aggr_addrs.size() < num_sides && dist < ROWS_PER_BANK; dist++) {
        if (victim_row >= dist + bank_start)
            t.aggr_addrs.push_back((victim_row - dist) * ROW_SIZE);
        if (t.aggr_addrs.size() < num_sides && victim_row + dist <= bank_start + ROWS_PER_BANK - 1)
            t.aggr_addrs.push_back((victim_row + dist) * ROW_SIZE);
    }
    return t;
}

int main(int argc, char** argv) {
    uint32_t cores_per_bank = 16;       // single-bank knee
    uint32_t start_row      = 2000;
    uint32_t num_sides      = 8;
    uint32_t hammer_iters   = 50000;
    uint32_t data_pattern   = 0x55555555;
    bool     dual_risc      = false;     // launch on both BRISC and NCRISC
    std::vector<uint32_t> sweep = {1, 2, 4, 8};

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--cores-per-bank") == 0 && i+1 < argc)
            cores_per_bank = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--start-row") == 0 && i+1 < argc)
            start_row = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--num-sides") == 0 && i+1 < argc)
            num_sides = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations") == 0 && i+1 < argc)
            hammer_iters = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--dual-risc") == 0)
            dual_risc = true;
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
            fmt::print("Usage: {} [--cores-per-bank N] [--start-row N] [--num-sides N]\n"
                       "              [--iterations N] [--sweep B1,B2,...] [--dual-risc]\n"
                       "  Each bank = one GDDR6 channel (independent die).\n"
                       "  Total cores = cores-per-bank * num-banks.\n"
                       "  --dual-risc launches on both BRISC and NCRISC per tile (2x threads).\n", argv[0]);
            return 0;
        }
    }

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];

    auto workers = compute_grid_workers(mesh_device.get());
    uint32_t max_workers = workers.size();

    // Cap sweep so cores_per_bank × max(banks) fits and banks ≤ 8 channels
    for (auto& B : sweep) {
        if (B > NUM_CHANNELS) B = NUM_CHANNELS;
        if (B * cores_per_bank > max_workers) B = max_workers / cores_per_bank;
    }
    std::vector<uint32_t> dedup;
    for (auto B : sweep) if (dedup.empty() || dedup.back() != B) dedup.push_back(B);
    sweep = std::move(dedup);

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t result_size  = RESULT_BUF_WORDS * sizeof(uint32_t);
    // BRISC layout
    uint32_t b_scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t b_result_addr  = (b_scratch_addr + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    // NCRISC layout (placed after BRISC's region)
    uint32_t n_scratch_addr = (b_result_addr + result_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_result_addr  = (n_scratch_addr + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;

    // Pre-compute one target per channel
    std::vector<BankTarget> all_targets;
    for (uint32_t c = 0; c < NUM_CHANNELS; c++) {
        all_targets.push_back(make_target(c, start_row, num_sides, min_safe_row));
    }

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Cross-Bank Scaling Sweep (FLUSH-READ, indep channels) ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Tiles per bank:   {}{}\n", cores_per_bank,
               dual_risc ? " (×2 RISCs/tile)" : "");
    fmt::print("║  Aggressors/bank:  {} (same-bank rows)\n", num_sides);
    fmt::print("║  Iterations:       {} per thread\n", hammer_iters);
    fmt::print("║  Worker pool:      {} tiles available\n", max_workers);
    fmt::print("║  Bank sweep:      ");
    for (auto B : sweep) fmt::print(" {}", B);
    fmt::print("\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    fmt::print("{:>6s} | {:>6s} | {:>14s} | {:>10s} | {:>14s} | {:>14s} | {:>10s} | {:>5s}\n",
               "banks", "cores", "total acts", "max ms", "per-bank Mas", "aggr Mas",
               "vs 1-bank", "flips");
    fmt::print("{:->6s}-+-{:->6s}-+-{:->14s}-+-{:->10s}-+-{:->14s}-+-{:->14s}-+-{:->10s}-+-{:->5s}\n",
               "", "", "", "", "", "", "", "");

    double baseline_per_bank = 0.0;

    for (uint32_t B : sweep) {
        uint32_t total_tiles = B * cores_per_bank;
        uint32_t per_tile_threads = dual_risc ? 2 : 1;
        std::vector<CoreCoord> sel(workers.begin(), workers.begin() + total_tiles);

        std::set<CoreRange> ranges;
        for (auto& c : sel) ranges.insert(CoreRange(c, c));
        CoreRangeSet core_set(ranges);

        Program program = CreateProgram();
        KernelHandle b_kid = CreateKernel(
            program,
            std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/core_scaling_kernel.cpp",
            core_set,
            DataMovementConfig{
                .processor = DataMovementProcessor::RISCV_0,
                .noc       = NOC::RISCV_0_default});
        KernelHandle n_kid = 0;
        if (dual_risc) {
            n_kid = CreateKernel(
                program,
                std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/core_scaling_kernel.cpp",
                core_set,
                DataMovementConfig{
                    .processor = DataMovementProcessor::RISCV_1,
                    .noc       = NOC::RISCV_1_default});
        }

        // Assign threads to banks: each tile contributes 1 (BRISC only) or 2
        // (BRISC + NCRISC) hammer threads. core_id 0 = primary (writes+verifies),
        // assigned to BRISC of tile 0 in bank 0. All other threads hammer-only.
        uint32_t thread_id = 0;
        for (uint32_t b = 0; b < B; b++) {
            const auto& tgt = all_targets[b];
            const auto& ch  = DRAM_CHANNELS[tgt.channel_idx];
            for (uint32_t i = 0; i < cores_per_bank; i++) {
                uint32_t global_tile_idx = b * cores_per_bank + i;
                // BRISC
                std::vector<uint32_t> b_args = {
                    ch.noc_x, ch.noc_y, tgt.victim_addr,
                    hammer_iters, data_pattern,
                    b_scratch_addr, b_result_addr,
                    thread_id,
                    static_cast<uint32_t>(tgt.aggr_addrs.size()),
                };
                for (auto a : tgt.aggr_addrs) b_args.push_back(a);
                SetRuntimeArgs(program, b_kid, sel[global_tile_idx], b_args);
                thread_id++;
                // NCRISC
                if (dual_risc) {
                    std::vector<uint32_t> n_args = {
                        ch.noc_x, ch.noc_y, tgt.victim_addr,
                        hammer_iters, data_pattern,
                        n_scratch_addr, n_result_addr,
                        thread_id,
                        static_cast<uint32_t>(tgt.aggr_addrs.size()),
                    };
                    for (auto a : tgt.aggr_addrs) n_args.push_back(a);
                    SetRuntimeArgs(program, n_kid, sel[global_tile_idx], n_args);
                    thread_id++;
                }
            }
        }

        distributed::MeshWorkload workload;
        workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                             std::move(program));
        distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
        distributed::Finish(cq);

        uint64_t total_acts = 0;
        uint64_t max_cycles = 0;
        std::vector<double> per_bank_rate(B, 0.0);
        std::vector<uint64_t> per_bank_acts(B, 0);
        std::vector<uint64_t> per_bank_max_cycles(B, 0);
        uint32_t total_flips = 0;
        auto absorb = [&](const std::vector<uint32_t>& rv, uint32_t b, bool primary) {
            uint32_t acts   = rv[5];
            uint64_t cycles = (static_cast<uint64_t>(rv[4]) << 32) | rv[3];
            total_acts += acts;
            per_bank_acts[b] += acts;
            if (cycles > max_cycles) max_cycles = cycles;
            if (cycles > per_bank_max_cycles[b]) per_bank_max_cycles[b] = cycles;
            if (primary) total_flips += rv[1];
        };
        for (uint32_t b = 0; b < B; b++) {
            for (uint32_t i = 0; i < cores_per_bank; i++) {
                uint32_t gi = b * cores_per_bank + i;
                std::vector<uint32_t> rv_b;
                detail::ReadFromDeviceL1(device, sel[gi], b_result_addr, result_size, rv_b);
                absorb(rv_b, b, b == 0 && i == 0);
                if (dual_risc) {
                    std::vector<uint32_t> rv_n;
                    detail::ReadFromDeviceL1(device, sel[gi], n_result_addr, result_size, rv_n);
                    absorb(rv_n, b, false);
                }
            }
        }
        for (uint32_t b = 0; b < B; b++) {
            if (per_bank_max_cycles[b] > 0)
                per_bank_rate[b] = per_bank_acts[b] /
                    (per_bank_max_cycles[b] * NS_PER_CYCLE / 1e9);
        }
        double avg_per_bank_M = 0;
        for (auto r : per_bank_rate) avg_per_bank_M += r;
        avg_per_bank_M = (avg_per_bank_M / B) / 1e6;
        double aggr_M = (max_cycles > 0)
            ? total_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6
            : 0.0;
        double max_ms = max_cycles * NS_PER_CYCLE / 1e6;

        if (B == sweep.front()) baseline_per_bank = avg_per_bank_M;
        double ratio = (baseline_per_bank > 0) ? avg_per_bank_M / baseline_per_bank : 1.0;

        uint32_t total_threads = total_tiles * per_tile_threads;
        fmt::print("{:>6d} | {:>6d} | {:>14d} | {:>10.1f} | {:>14.3f} | {:>14.3f} | {:>9.2f}x | {:>5d}\n",
                   B, total_threads, static_cast<int>(total_acts), max_ms,
                   avg_per_bank_M, aggr_M, ratio, total_flips);
    }

    fmt::print("\nInterpretation:\n");
    fmt::print("  per-bank rate flat as B grows  -> banks are independent; aggregate scales linearly.\n");
    fmt::print("  per-bank rate falls            -> shared upstream bottleneck (controller front-end / NOC / PCIe).\n");
    fmt::print("  aggregate plateau              -> upper bound on real act/s reachable from user-space.\n");

    mesh_device->close();
    return 0;
}
