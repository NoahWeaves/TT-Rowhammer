// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Bank-boundary verification probe.
//
// We've assumed ROWS_PER_BANK = 16 from R4 latency tiering. But our 80 M
// act/s measurement on a "single bank" exceeds GDDR6 tRC physical limit
// (~25 M act/s on one true physical bank). Either we're miscounting, or
// our "single bank" actually spans multiple physical banks via interleaving.
//
// Direct test: hammer 2 aggressor rows with controllable spacing using
// FLUSH-READ (ground-truth real activations). Measure aggregate rate.
//   - If both rows are in the same physical bank, controller MUST take tRC
//     (~32 ns) between activations → rate capped at ~50 M act/s for two rows
//     = 25 M per row.
//   - If they're in different physical banks, controller can pipeline ACTs
//     across banks → rate much higher (~80 M aggregate possible).
//
// Sweep spacing ∈ {1, 2, 4, 8, 16, 32, 64, 128, 256} rows. The transition
// where rate JUMPS UP marks the physical bank boundary.
//
// We use ONE sub-port and 16 tiles dual-RISC = 32 threads (per-bank knee).

#include <algorithm>
#include <array>
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
static constexpr uint32_t L1_ALIGN        = 64;

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

struct SubPort { uint32_t noc_x; uint32_t noc_y; };
struct ChannelEndpoints { const char* name; std::array<SubPort, 3> sp; };
static const ChannelEndpoints CHANNELS[8] = {
    {"ch0", {{ {0,0}, {0,1},  {0,11} }}},
    {"ch1", {{ {0,2}, {0,10}, {0,3}  }}},
    {"ch2", {{ {0,9}, {0,4},  {0,8}  }}},
    {"ch3", {{ {0,5}, {0,7},  {0,6}  }}},
    {"ch4", {{ {9,0}, {9,1},  {9,11} }}},
    {"ch5", {{ {9,2}, {9,10}, {9,3}  }}},
    {"ch6", {{ {9,9}, {9,4},  {9,8}  }}},
    {"ch7", {{ {9,5}, {9,7},  {9,6}  }}},
};

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

static std::vector<CoreCoord> compute_grid_workers(distributed::MeshDevice* mesh) {
    CoreCoord g = mesh->compute_with_storage_grid_size();
    std::vector<CoreCoord> out; out.reserve(g.x * g.y);
    for (uint32_t y = 0; y < g.y; y++)
        for (uint32_t x = 0; x < g.x; x++) out.push_back({x, y});
    return out;
}

int main(int argc, char** argv) {
    uint32_t channel_idx  = 0;
    uint32_t subport_idx  = 1;
    uint32_t num_subports = 3;             // engage all 3 by default for max controller pressure
    uint32_t tiles_per_sp = 16;            // 16 tiles dual-RISC per sub-port
    uint32_t base_row     = 4096;
    uint32_t hammer_iters = 200000;
    uint32_t data_pattern = 0x55555555;
    std::vector<uint32_t> spacings = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--channel") && i+1 < argc)        channel_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sub-port") && i+1 < argc)  subport_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sub-ports") && i+1 < argc) num_subports = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiles-per-sp") && i+1 < argc) tiles_per_sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiles") && i+1 < argc)     tiles_per_sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--base-row") && i+1 < argc)  base_row = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations") && i+1 < argc) hammer_iters = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--spacings") && i+1 < argc) {
            spacings.clear();
            const char* s = argv[++i];
            while (*s) {
                char* e; uint32_t v = std::strtoul(s, &e, 10);
                if (e == s) break; spacings.push_back(v); s = e;
                if (*s == ',') s++;
            }
        }
    }
    if (channel_idx >= 8 || subport_idx >= 3) return 1;
    if (num_subports < 1) num_subports = 1;
    if (num_subports > 3) num_subports = 3;
    const auto& ch = CHANNELS[channel_idx];

    std::vector<uint32_t> active_sp_idx;
    if (num_subports == 1) active_sp_idx.push_back(subport_idx);
    else for (uint32_t s = 0; s < num_subports; s++) active_sp_idx.push_back(s);
    uint32_t total_tiles = num_subports * tiles_per_sp;

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq  = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];
    auto workers = compute_grid_workers(mesh.get());
    if (workers.size() < total_tiles) return 1;

    uint32_t l1_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t result_size = RESULT_BUF_WORDS * sizeof(uint32_t);
    uint32_t b_scratch = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t b_result  = (b_scratch + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_scratch = (b_result + result_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_result  = (n_scratch + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (base_row < min_safe_row) base_row = min_safe_row;

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Bank-Boundary Verification Probe                      ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:        {} sub-ports engaged: ", ch.name);
    for (auto si : active_sp_idx) fmt::print("({},{}) ", ch.sp[si].noc_x, ch.sp[si].noc_y);
    fmt::print("\n");
    fmt::print("║  Tiles/sub-port: {} (× 2 RISCs each)\n", tiles_per_sp);
    fmt::print("║  Total threads:  {}\n", total_tiles * 2);
    fmt::print("║  Base row:       {}\n", base_row);
    fmt::print("║  Iters/thread:   {} (× 2 aggressors = {} acts/thread)\n",
               hammer_iters, hammer_iters * 2);
    fmt::print("║  Aggressors:     2 rows at base_row and base_row+SPACING\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    fmt::print("If our ROWS_PER_BANK=16 model is right:\n");
    fmt::print("  spacing=1..15  → same bank → controller serializes ACTs at tRC\n");
    fmt::print("  spacing=16+    → different banks → controller pipelines → higher rate\n\n");

    fmt::print("{:>8s} | {:>10s} | {:>10s} | {:>14s} | {:>10s} | {:>14s}\n",
               "spacing", "row_A", "row_B", "total acts", "max ms", "aggr Mas");
    fmt::print("{:->8s}-+-{:->10s}-+-{:->10s}-+-{:->14s}-+-{:->10s}-+-{:->14s}\n",
               "", "", "", "", "", "");

    for (uint32_t spacing : spacings) {
        uint32_t row_a = base_row;
        uint32_t row_b = base_row + spacing;
        uint32_t addr_a = row_a * ROW_SIZE;
        uint32_t addr_b = row_b * ROW_SIZE;

        std::vector<CoreCoord> sel(workers.begin(), workers.begin() + total_tiles);
        std::set<CoreRange> ranges;
        for (auto& c : sel) ranges.insert(CoreRange(c, c));
        CoreRangeSet core_set(ranges);

        Program prog = CreateProgram();
        KernelHandle b_kid = CreateKernel(
            prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/core_scaling_kernel.cpp",
            core_set,
            DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                                .noc       = NOC::RISCV_0_default });
        KernelHandle n_kid = CreateKernel(
            prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/core_scaling_kernel.cpp",
            core_set,
            DataMovementConfig{ .processor = DataMovementProcessor::RISCV_1,
                                .noc       = NOC::RISCV_1_default });

        // Distribute tiles across active sub-ports.
        for (uint32_t s = 0; s < num_subports; s++) {
            const auto& sp = ch.sp[active_sp_idx[s]];
            for (uint32_t i = 0; i < tiles_per_sp; i++) {
                uint32_t global_tile = s * tiles_per_sp + i;
                bool is_primary = (s == 0 && i == 0);
                uint32_t b_core_id = is_primary ? 0 : (global_tile * 2 + 100);
                uint32_t n_core_id = global_tile * 2 + 101;
                std::vector<uint32_t> b_args = {
                    sp.noc_x, sp.noc_y, addr_a,
                    hammer_iters, data_pattern,
                    b_scratch, b_result,
                    b_core_id, 2u,
                    addr_a, addr_b,
                };
                SetRuntimeArgs(prog, b_kid, sel[global_tile], b_args);
                std::vector<uint32_t> n_args = {
                    sp.noc_x, sp.noc_y, addr_a,
                    hammer_iters, data_pattern,
                    n_scratch, n_result,
                    n_core_id, 2u,
                    addr_a, addr_b,
                };
                SetRuntimeArgs(prog, n_kid, sel[global_tile], n_args);
            }
        }

        distributed::MeshWorkload wl;
        wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
        distributed::EnqueueMeshWorkload(cq, wl, false);
        distributed::Finish(cq);

        uint64_t total_acts = 0, max_cycles = 0;
        for (uint32_t i = 0; i < total_tiles; i++) {
            std::vector<uint32_t> rb, rn;
            detail::ReadFromDeviceL1(dev, sel[i], b_result, result_size, rb);
            detail::ReadFromDeviceL1(dev, sel[i], n_result, result_size, rn);
            total_acts += rb[5] + rn[5];
            uint64_t cyc_b = (static_cast<uint64_t>(rb[4]) << 32) | rb[3];
            uint64_t cyc_n = (static_cast<uint64_t>(rn[4]) << 32) | rn[3];
            if (cyc_b > max_cycles) max_cycles = cyc_b;
            if (cyc_n > max_cycles) max_cycles = cyc_n;
        }
        double aggr_M = (max_cycles > 0)
            ? total_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6 : 0.0;
        double max_ms = max_cycles * NS_PER_CYCLE / 1e6;

        fmt::print("{:>8d} | {:>10d} | {:>10d} | {:>14d} | {:>10.1f} | {:>14.3f}\n",
                   spacing, row_a, row_b, static_cast<int>(total_acts), max_ms, aggr_M);
    }

    fmt::print("\nInterpretation:\n");
    fmt::print("  rate flat across all spacings              → all spacings hit different banks (no boundary)\n");
    fmt::print("  rate jumps UP at spacing=16                → confirms ROWS_PER_BANK=16 model\n");
    fmt::print("  rate jumps UP at spacing=4,8 (smaller)     → real banks are smaller; our 'bank' is a bank GROUP\n");
    fmt::print("  rate flat at low spacings, jumps later     → real banks are larger than 16 rows\n");

    mesh->close();
    return 0;
}
