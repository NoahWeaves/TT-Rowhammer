// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// NOC sub-port scaling sweep.
//
// Each Blackhole GDDR6 channel is exposed at THREE NOC endpoints (sub-ports).
// All prior rowhammer experiments here have hit only the middle endpoint of
// each channel, so we have potentially up to 3× per-channel headroom that's
// never been measured. This sweep answers: are the 3 sub-ports independent
// paths to the per-channel controller (linear scaling) or just NOC routes to
// the same queue (no scaling)?
//
// Method: pin to ONE channel, hammer the SAME victim row from `cores_per_sp`
// tiles per engaged sub-port (dual-RISC), sweep how many sub-ports are
// engaged ∈ {1, 2, 3}. Real activations counted via FLUSH-READ.
//
// Channel 0 example: sub-ports = (0,0), (0,1), (0,11). Hitting the same DRAM
// row through 2 or 3 of them tests whether the controller serves them in
// parallel.

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
static constexpr uint32_t ROWS_PER_BANK   = 16;

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
static constexpr uint32_t L1_ALIGN          = 64;

// From blackhole_140_arch.yaml `dram:` section. Each row = 3 sub-port endpoints
// for one physical GDDR6 channel.
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
    uint32_t channel_idx    = 0;
    uint32_t cores_per_sp   = 16;       // tiles per sub-port (× 2 RISCs = threads)
    uint32_t start_row      = 2000;
    uint32_t num_sides      = 8;
    uint32_t hammer_iters   = 50000;
    uint32_t data_pattern   = 0x55555555;
    std::vector<uint32_t> sweep = {1, 2, 3};

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--channel") && i+1 < argc)         channel_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cores-per-sp") && i+1 < argc) cores_per_sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--start-row") && i+1 < argc)   start_row = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--num-sides") && i+1 < argc)   num_sides = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations") && i+1 < argc)  hammer_iters = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sweep") && i+1 < argc) {
            sweep.clear();
            const char* s = argv[++i];
            while (*s) {
                char* e; uint32_t v = std::strtoul(s, &e, 10);
                if (e == s) break;
                sweep.push_back(v);
                s = e;
                if (*s == ',') s++;
            }
        }
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--cores-per-sp N]\n"
                       "              [--start-row N] [--num-sides N]\n"
                       "              [--iterations N] [--sweep S1,S2,...]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) { fmt::print(stderr, "channel out of range\n"); return 1; }
    const auto& ch = CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq  = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];
    auto workers = compute_grid_workers(mesh.get());
    uint32_t max_workers = workers.size();

    for (auto& S : sweep) { if (S > 3) S = 3; if (S * cores_per_sp > max_workers) S = max_workers / cores_per_sp; }
    std::vector<uint32_t> dedup;
    for (auto S : sweep) if (dedup.empty() || dedup.back() != S) dedup.push_back(S);
    sweep = std::move(dedup);

    uint32_t l1_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t result_size = RESULT_BUF_WORDS * sizeof(uint32_t);
    uint32_t b_scratch = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t b_result  = (b_scratch + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_scratch = (b_result + result_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_result  = (n_scratch + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (start_row < min_safe_row) start_row = min_safe_row;

    uint32_t bank_start = (start_row / ROWS_PER_BANK) * ROWS_PER_BANK;
    uint32_t victim_row = bank_start + ROWS_PER_BANK / 2;
    uint32_t victim_addr = victim_row * ROW_SIZE;

    std::vector<uint32_t> aggr_addrs;
    for (uint32_t d = 1; aggr_addrs.size() < num_sides && d < ROWS_PER_BANK; d++) {
        if (victim_row >= d + bank_start)
            aggr_addrs.push_back((victim_row - d) * ROW_SIZE);
        if (aggr_addrs.size() < num_sides && victim_row + d <= bank_start + ROWS_PER_BANK - 1)
            aggr_addrs.push_back((victim_row + d) * ROW_SIZE);
    }

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       NOC Sub-port Scaling Sweep ({}, dual-RISC)             ║\n", ch.name);
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:         {} sub-ports: ({},{}) ({},{}) ({},{})\n",
               ch.name, ch.sp[0].noc_x, ch.sp[0].noc_y,
               ch.sp[1].noc_x, ch.sp[1].noc_y,
               ch.sp[2].noc_x, ch.sp[2].noc_y);
    fmt::print("║  Tiles/sub-port:  {} (× 2 RISCs)\n", cores_per_sp);
    fmt::print("║  Victim row:      {} (0x{:08x})\n", victim_row, victim_addr);
    fmt::print("║  Aggressors:      {}\n", aggr_addrs.size());
    fmt::print("║  Iterations:      {} per thread\n", hammer_iters);
    fmt::print("║  Sweep:          ");
    for (auto S : sweep) fmt::print(" {}", S);
    fmt::print("\n╚══════════════════════════════════════════════════════════════╝\n\n");

    fmt::print("{:>6s} | {:>8s} | {:>14s} | {:>10s} | {:>14s} | {:>14s} | {:>10s}\n",
               "subps", "threads", "total acts", "max ms", "per-sp Mas", "aggr Mas", "vs 1-sp");
    fmt::print("{:->6s}-+-{:->8s}-+-{:->14s}-+-{:->10s}-+-{:->14s}-+-{:->14s}-+-{:->10s}\n",
               "", "", "", "", "", "", "");

    double baseline_aggr = 0.0;

    for (uint32_t S : sweep) {
        uint32_t total_tiles = S * cores_per_sp;
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

        // Each sub-port gets cores_per_sp tiles assigned. All tiles target the
        // same victim_addr / aggr_addrs; only the (noc_x, noc_y) differs.
        // First tile of first engaged sub-port gets BRISC core_id=0 (writes/verifies).
        for (uint32_t s = 0; s < S; s++) {
            const auto& sp = ch.sp[s];
            for (uint32_t i = 0; i < cores_per_sp; i++) {
                uint32_t global_tile = s * cores_per_sp + i;
                uint32_t b_core_id = (s == 0 && i == 0) ? 0 : (global_tile * 2 + 100);
                uint32_t n_core_id = global_tile * 2 + 101;
                std::vector<uint32_t> b_args = {
                    sp.noc_x, sp.noc_y, victim_addr,
                    hammer_iters, data_pattern,
                    b_scratch, b_result,
                    b_core_id,
                    static_cast<uint32_t>(aggr_addrs.size()),
                };
                for (auto a : aggr_addrs) b_args.push_back(a);
                SetRuntimeArgs(prog, b_kid, sel[global_tile], b_args);

                std::vector<uint32_t> n_args = {
                    sp.noc_x, sp.noc_y, victim_addr,
                    hammer_iters, data_pattern,
                    n_scratch, n_result,
                    n_core_id,
                    static_cast<uint32_t>(aggr_addrs.size()),
                };
                for (auto a : aggr_addrs) n_args.push_back(a);
                SetRuntimeArgs(prog, n_kid, sel[global_tile], n_args);
            }
        }

        distributed::MeshWorkload wl;
        wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
        distributed::EnqueueMeshWorkload(cq, wl, false);
        distributed::Finish(cq);

        // Per-sub-port aggregation
        std::vector<uint64_t> sp_acts(S, 0), sp_max_cycles(S, 0);
        uint64_t total_acts = 0, max_cycles = 0;
        for (uint32_t s = 0; s < S; s++) {
            for (uint32_t i = 0; i < cores_per_sp; i++) {
                uint32_t gi = s * cores_per_sp + i;
                std::vector<uint32_t> rb, rn;
                detail::ReadFromDeviceL1(dev, sel[gi], b_result, result_size, rb);
                detail::ReadFromDeviceL1(dev, sel[gi], n_result, result_size, rn);
                auto absorb = [&](const std::vector<uint32_t>& rv) {
                    uint32_t a = rv[5];
                    uint64_t c = (static_cast<uint64_t>(rv[4]) << 32) | rv[3];
                    sp_acts[s] += a;
                    total_acts += a;
                    if (c > sp_max_cycles[s]) sp_max_cycles[s] = c;
                    if (c > max_cycles) max_cycles = c;
                };
                absorb(rb); absorb(rn);
            }
        }
        double avg_per_sp_M = 0;
        for (uint32_t s = 0; s < S; s++) {
            if (sp_max_cycles[s] > 0)
                avg_per_sp_M += sp_acts[s] / (sp_max_cycles[s] * NS_PER_CYCLE / 1e9);
        }
        avg_per_sp_M = (avg_per_sp_M / S) / 1e6;
        double aggr_M = (max_cycles > 0)
            ? total_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6 : 0.0;
        double max_ms = max_cycles * NS_PER_CYCLE / 1e6;

        if (S == sweep.front()) baseline_aggr = aggr_M;
        double ratio = (baseline_aggr > 0) ? aggr_M / baseline_aggr : 1.0;

        uint32_t threads = total_tiles * 2;
        fmt::print("{:>6d} | {:>8d} | {:>14d} | {:>10.1f} | {:>14.3f} | {:>14.3f} | {:>9.2f}x\n",
                   S, threads, static_cast<int>(total_acts), max_ms,
                   avg_per_sp_M, aggr_M, ratio);
    }

    fmt::print("\nInterpretation:\n");
    fmt::print("  aggr scales linearly with sub-ports → sub-ports are independent paths to controller\n");
    fmt::print("  aggr stays flat / per-sp falls 1/S  → sub-ports share controller queue (NOC routes only)\n");
    fmt::print("  intermediate scaling                → partial independence (e.g. shared command bus)\n");

    mesh->close();
    return 0;
}
