// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Multi-thread row-cycle sweep — 96-thread version of row_cycle_sweep.
//
// Spawns the long_hammer / peak_attack peak config (1 channel × 3 sub-ports
// × 16 tiles × dual-RISC = 96 threads) targeting one bank, and sweeps N =
// number of distinct rows cycled through pipelined K=16 reads.
//
// Single-thread row_cycle showed N=1 → 14 M reads/s (all hits) and N=2..64 →
// ~11 M reads/s (all misses, every read = real ACT). This driver tests
// whether that knee survives 96-thread contention on the same bank.
//
// Output per N: aggregate M reads/s across 96 threads, per-thread average,
// total NIU MST req delta, kernel-counted reads, NIU:reads ratio.
//
// Reuses access_pattern_kernel.cpp mode 7.

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

static constexpr uint32_t ROW_SIZE     = 8192;
static constexpr uint32_t CACHELINE    = 64;
static constexpr double   NS_PER_CYCLE = 1.25;
static constexpr uint32_t L1_ALIGN     = 64;
static constexpr uint32_t RESULT_WORDS = 8;
static constexpr uint32_t SCRATCH_CL   = 8;  // mode 7 uses scratch_a + (i&7)*CL

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

struct Aggregate {
    uint64_t total_reads;
    uint64_t total_niu_req;
    uint64_t total_niu_resp;
    uint64_t max_cycles;
    uint64_t min_cycles;
};

int main(int argc, char** argv) {
    uint32_t channel_idx  = 0;
    uint32_t tiles_per_sp = 16;
    uint32_t base_row     = 30000;
    uint32_t iterations   = 200000;  // per-thread; enough to dominate startup

    for (int i = 1; i < argc; i++) {
        if      (!std::strcmp(argv[i], "--channel")      && i+1 < argc) channel_idx  = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiles-per-sp") && i+1 < argc) tiles_per_sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--base-row")     && i+1 < argc) base_row     = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations")   && i+1 < argc) iterations   = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--tiles-per-sp N] [--base-row N] [--iterations N]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) return 1;
    const auto& ch = CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq  = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];
    auto workers = compute_grid_workers(mesh.get());
    uint32_t need_tiles = 3 * tiles_per_sp;
    if (workers.size() < need_tiles) {
        fmt::print(stderr, "ERROR: need {} tiles but only {} available\n", need_tiles, workers.size());
        return 1;
    }

    uint32_t l1_base   = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t result_size = RESULT_WORDS * sizeof(uint32_t);
    uint32_t b_scratch = (l1_base   + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t b_result  = (b_scratch + SCRATCH_CL * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_scratch = (b_result  + result_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_result  = (n_scratch + SCRATCH_CL * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (base_row < min_safe_row) base_row = min_safe_row;
    uint32_t base_addr = base_row * ROW_SIZE;

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Multi-thread Row-Cycle Sweep (mode 7, K=16 pipelined)\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:           {} all 3 sub-ports\n", ch.name);
    fmt::print("║  Tiles/sub-port:    {} (× 2 RISCs = {} threads)\n", tiles_per_sp, 3 * tiles_per_sp * 2);
    fmt::print("║  Base row:          {} (0x{:08x})\n", base_row, base_addr);
    fmt::print("║  Iterations/thread: {}\n", iterations);
    fmt::print("║  Reads/thread:      iterations × 16 (K=16 burst)\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    std::vector<CoreCoord> sel(workers.begin(), workers.begin() + need_tiles);
    std::set<CoreRange> ranges;
    for (auto& c : sel) ranges.insert(CoreRange(c, c));
    CoreRangeSet core_set(ranges);

    fmt::print("{:>4} {:>14} {:>14} {:>14} {:>10} {:>10} {:>11}  interpretation\n",
               "N", "kernel reads", "NIU MST req", "NIU MST resp",
               "M reads/s", "per-thr", "NIU:reads");
    fmt::print("{:-<120}\n", "");

    static const uint32_t N_VALUES[] = {1, 2, 4, 8, 16, 32};

    Aggregate base_n1{};

    for (uint32_t n_rows : N_VALUES) {
        Program prog = CreateProgram();
        KernelHandle b_kid = CreateKernel(
            prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/access_pattern_kernel.cpp",
            core_set,
            DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                                .noc       = NOC::RISCV_0_default });
        KernelHandle n_kid = CreateKernel(
            prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/access_pattern_kernel.cpp",
            core_set,
            DataMovementConfig{ .processor = DataMovementProcessor::RISCV_1,
                                .noc       = NOC::RISCV_1_default });

        for (uint32_t s = 0; s < 3; s++) {
            const auto& sp = ch.sp[s];
            for (uint32_t i = 0; i < tiles_per_sp; i++) {
                uint32_t gi = s * tiles_per_sp + i;
                std::vector<uint32_t> b_args = {
                    sp.noc_x, sp.noc_y, base_addr, iterations, /*mode*/7u,
                    b_scratch, b_result, n_rows
                };
                std::vector<uint32_t> n_args = {
                    sp.noc_x, sp.noc_y, base_addr, iterations, /*mode*/7u,
                    n_scratch, n_result, n_rows
                };
                SetRuntimeArgs(prog, b_kid, sel[gi], b_args);
                SetRuntimeArgs(prog, n_kid, sel[gi], n_args);
            }
        }

        distributed::MeshWorkload wl;
        wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
        distributed::EnqueueMeshWorkload(cq, wl, false);
        distributed::Finish(cq);

        Aggregate agg{0, 0, 0, 0, UINT64_MAX};
        for (uint32_t s = 0; s < 3; s++) {
            for (uint32_t i = 0; i < tiles_per_sp; i++) {
                uint32_t gi = s * tiles_per_sp + i;
                std::vector<uint32_t> rb, rn;
                detail::ReadFromDeviceL1(dev, sel[gi], b_result, result_size, rb);
                detail::ReadFromDeviceL1(dev, sel[gi], n_result, result_size, rn);
                uint64_t b_cyc = (static_cast<uint64_t>(rb[1]) << 32) | rb[0];
                uint64_t n_cyc = (static_cast<uint64_t>(rn[1]) << 32) | rn[0];
                agg.total_reads += rb[2] + rn[2];
                agg.total_niu_req  += rb[4] + rn[4];
                agg.total_niu_resp += rb[5] + rn[5];
                agg.max_cycles = std::max(agg.max_cycles, std::max(b_cyc, n_cyc));
                if (b_cyc > 0) agg.min_cycles = std::min(agg.min_cycles, b_cyc);
                if (n_cyc > 0) agg.min_cycles = std::min(agg.min_cycles, n_cyc);
            }
        }
        if (agg.min_cycles == UINT64_MAX) agg.min_cycles = 0;
        if (n_rows == 1) base_n1 = agg;

        double sec = agg.max_cycles * NS_PER_CYCLE / 1e9;
        double aggr_M = sec > 0 ? agg.total_reads / sec / 1e6 : 0;
        double per_thr_M = aggr_M / (3.0 * tiles_per_sp * 2);
        double niu_ratio = agg.total_reads > 0 ? double(agg.total_niu_req) / double(agg.total_reads) : 0;

        const char* note = "all hits (cached)";
        if (n_rows == 1) {
            note = "<— same-row baseline";
        } else {
            double rel = base_n1.max_cycles > 0
                ? double(agg.max_cycles) / double(base_n1.max_cycles) : 1.0;
            if (rel < 1.05)       note = "no slowdown vs N=1 (controller reorders OR row-buffer >1?)";
            else if (rel < 1.40)  note = "moderate slowdown (mixed hit/miss)";
            else                  note = "<— KNEE: every read missed";
        }

        fmt::print("{:>4} {:>14} {:>14} {:>14} {:>10.2f} {:>10.3f} {:>11.3f}  {}\n",
                   n_rows, agg.total_reads, agg.total_niu_req, agg.total_niu_resp,
                   aggr_M, per_thr_M, niu_ratio, note);
    }

    fmt::print("\n[Interpretation]\n");
    fmt::print("  N=1 = same-row pipelined; row buffer hits all reads, only one ACT.\n");
    fmt::print("  N≥2 with no slowdown   → controller reorders / row buffer > 1 row.\n");
    fmt::print("  N≥2 with strong slowdown → every read is a real ACT (rowhammer-relevant).\n");
    fmt::print("  NIU:reads ratio < 1.0  → NOC/NIU coalesces kernel calls into wider transactions.\n");
    fmt::print("  NIU:reads ratio = 1.0  → every kernel noc_async_read is a separate NIU transaction.\n");

    mesh->close();
    return 0;
}
