// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Within-channel multi-bank scaling sweep.
//
// Pins to ONE channel and ONE sub-port. Sweeps the number of "internal
// banks" (N) hammered in parallel: each internal bank gets its own victim
// row separated by ROWS_PER_BANK * ROW_SIZE = 128 KB so addresses fall in
// different bank IDs per the latency-derived address mapping.
//
// Hypotheses:
//   - If the channel command bus is the bottleneck → aggregate stays at the
//     single-bank ceiling (~28 M) regardless of N.
//   - If internal banks are independent → aggregate scales N×.
//   - Real GDDR6 bank-interleaved access usually gives partial scaling
//     (banks have independent state machines but share one command bus, so
//     theoretical 2-4× headroom).

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
static constexpr uint32_t ROWS_PER_BANK   = 16;     // from latency mapping
static constexpr uint32_t BANK_STRIDE_ROWS = ROWS_PER_BANK;  // 16 rows = 128 KB

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;
static constexpr uint32_t L1_ALIGN          = 64;

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
    uint32_t subport_idx    = 1;        // single sub-port mode: which one
    uint32_t num_subports   = 1;        // multi sub-port mode: how many to engage
    uint32_t tiles_per_bank = 16;
    uint32_t start_row      = 2048;
    uint32_t num_sides      = 8;
    uint32_t hammer_iters   = 50000;
    uint32_t data_pattern   = 0x55555555;
    std::vector<uint32_t> sweep = {1, 2, 4, 8};

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--channel") && i+1 < argc)        channel_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sub-port") && i+1 < argc)  subport_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sub-ports") && i+1 < argc) num_subports = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiles-per-bank") && i+1 < argc) tiles_per_bank = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--start-row") && i+1 < argc) start_row = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--num-sides") && i+1 < argc) num_sides = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations") && i+1 < argc) hammer_iters = std::atoi(argv[++i]);
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
            fmt::print("Usage: {} [--channel N] [--sub-port {{0,1,2}}] [--tiles-per-bank N]\n"
                       "              [--start-row N] [--num-sides N] [--iterations N]\n"
                       "              [--sweep N1,N2,...]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8 || subport_idx >= 3) return 1;
    if (num_subports < 1) num_subports = 1;
    if (num_subports > 3) num_subports = 3;
    const auto& ch = CHANNELS[channel_idx];

    // Active sub-ports: single mode → just the chosen index; multi mode → first K.
    std::vector<uint32_t> active_sp_idx;
    if (num_subports == 1) active_sp_idx.push_back(subport_idx);
    else for (uint32_t s = 0; s < num_subports; s++) active_sp_idx.push_back(s);
    uint32_t tiles_per_sp_per_bank = tiles_per_bank / num_subports;
    uint32_t real_tiles_per_bank   = tiles_per_sp_per_bank * num_subports;

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq  = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];
    auto workers = compute_grid_workers(mesh.get());
    uint32_t max_workers = workers.size();

    for (auto& N : sweep) { if (N * real_tiles_per_bank > max_workers) N = max_workers / real_tiles_per_bank; }
    std::vector<uint32_t> dedup;
    for (auto N : sweep) if (dedup.empty() || dedup.back() != N) dedup.push_back(N);
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

    // Build per-internal-bank targets: each bank b gets victim row at
    // start_row + b * BANK_STRIDE_ROWS, centered within its 16-row bank.
    // Ensure each victim is centered in its own bank (so aggressors fit).
    auto bank_target = [&](uint32_t b) {
        uint32_t bs = ((start_row + b * BANK_STRIDE_ROWS) / ROWS_PER_BANK) * ROWS_PER_BANK;
        uint32_t v_row = bs + ROWS_PER_BANK / 2;
        std::vector<uint32_t> aggrs;
        for (uint32_t d = 1; aggrs.size() < num_sides && d < ROWS_PER_BANK; d++) {
            if (v_row >= d + bs) aggrs.push_back((v_row - d) * ROW_SIZE);
            if (aggrs.size() < num_sides && v_row + d <= bs + ROWS_PER_BANK - 1)
                aggrs.push_back((v_row + d) * ROW_SIZE);
        }
        return std::pair<uint32_t, std::vector<uint32_t>>{ v_row * ROW_SIZE, std::move(aggrs) };
    };

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Within-Channel Multi-Bank Sweep ({})                    ║\n", ch.name);
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Sub-ports:      {} (engaged: ", num_subports);
    for (auto si : active_sp_idx) fmt::print("({},{}) ", ch.sp[si].noc_x, ch.sp[si].noc_y);
    fmt::print(")\n");
    fmt::print("║  Tiles/bank:     {} (× 2 RISCs, split {} per sub-port)\n",
               real_tiles_per_bank, tiles_per_sp_per_bank);
    fmt::print("║  Bank stride:    {} rows ({} KB)\n", BANK_STRIDE_ROWS, BANK_STRIDE_ROWS * ROW_SIZE / 1024);
    fmt::print("║  Aggressors:     {}\n", num_sides);
    fmt::print("║  Iterations:     {} per thread\n", hammer_iters);
    fmt::print("║  Sweep:         ");
    for (auto N : sweep) fmt::print(" {}", N);
    fmt::print("\n╚══════════════════════════════════════════════════════════════╝\n\n");

    // Show victim row plan
    fmt::print("Victim rows (one per internal bank):\n");
    for (uint32_t b = 0; b < sweep.back(); b++) {
        auto [vaddr, aggrs] = bank_target(b);
        fmt::print("  ibank{}: victim row {} (0x{:08x})\n", b, vaddr / ROW_SIZE, vaddr);
    }
    fmt::print("\n");

    fmt::print("{:>7s} | {:>8s} | {:>14s} | {:>10s} | {:>14s} | {:>14s} | {:>10s}\n",
               "ibanks", "threads", "total acts", "max ms", "per-bank Mas", "aggr Mas", "vs 1-bank");
    fmt::print("{:->7s}-+-{:->8s}-+-{:->14s}-+-{:->10s}-+-{:->14s}-+-{:->14s}-+-{:->10s}\n",
               "", "", "", "", "", "", "");

    double baseline_per_bank = 0.0;

    for (uint32_t N : sweep) {
        uint32_t total_tiles = N * real_tiles_per_bank;
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

        // Per internal-bank b: real_tiles_per_bank tiles total, distributed
        // across active sub-ports. Tile layout within bank:
        //   [sp0 × tiles_per_sp_per_bank][sp1 × tiles_per_sp_per_bank]...
        // Only ibank-0's first tile (BRISC) gets core_id=0 (writes + verifies).
        for (uint32_t b = 0; b < N; b++) {
            auto [v_addr, aggrs] = bank_target(b);
            for (uint32_t s = 0; s < num_subports; s++) {
                const auto& sp = ch.sp[active_sp_idx[s]];
                for (uint32_t i = 0; i < tiles_per_sp_per_bank; i++) {
                    uint32_t global_tile = b * real_tiles_per_bank
                                         + s * tiles_per_sp_per_bank + i;
                    bool is_primary = (b == 0 && s == 0 && i == 0);
                    uint32_t b_core_id = is_primary ? 0 : (global_tile * 2 + 100);
                    uint32_t n_core_id = global_tile * 2 + 101;
                    std::vector<uint32_t> b_args = {
                        sp.noc_x, sp.noc_y, v_addr,
                        hammer_iters, data_pattern,
                        b_scratch, b_result,
                        b_core_id,
                        static_cast<uint32_t>(aggrs.size()),
                    };
                    for (auto a : aggrs) b_args.push_back(a);
                    SetRuntimeArgs(prog, b_kid, sel[global_tile], b_args);

                    std::vector<uint32_t> n_args = {
                        sp.noc_x, sp.noc_y, v_addr,
                        hammer_iters, data_pattern,
                        n_scratch, n_result,
                        n_core_id,
                        static_cast<uint32_t>(aggrs.size()),
                    };
                    for (auto a : aggrs) n_args.push_back(a);
                    SetRuntimeArgs(prog, n_kid, sel[global_tile], n_args);
                }
            }
        }

        distributed::MeshWorkload wl;
        wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
        distributed::EnqueueMeshWorkload(cq, wl, false);
        distributed::Finish(cq);

        std::vector<uint64_t> bk_acts(N, 0), bk_max_cycles(N, 0);
        uint64_t total_acts = 0, max_cycles = 0;
        for (uint32_t b = 0; b < N; b++) {
            for (uint32_t i = 0; i < real_tiles_per_bank; i++) {
                uint32_t gi = b * real_tiles_per_bank + i;
                std::vector<uint32_t> rb, rn;
                detail::ReadFromDeviceL1(dev, sel[gi], b_result, result_size, rb);
                detail::ReadFromDeviceL1(dev, sel[gi], n_result, result_size, rn);
                auto absorb = [&](const std::vector<uint32_t>& rv) {
                    uint32_t a = rv[5];
                    uint64_t c = (static_cast<uint64_t>(rv[4]) << 32) | rv[3];
                    bk_acts[b] += a;
                    total_acts += a;
                    if (c > bk_max_cycles[b]) bk_max_cycles[b] = c;
                    if (c > max_cycles) max_cycles = c;
                };
                absorb(rb); absorb(rn);
            }
        }
        double avg_per_bank_M = 0;
        for (uint32_t b = 0; b < N; b++) {
            if (bk_max_cycles[b] > 0)
                avg_per_bank_M += bk_acts[b] / (bk_max_cycles[b] * NS_PER_CYCLE / 1e9);
        }
        avg_per_bank_M = (avg_per_bank_M / N) / 1e6;
        double aggr_M = (max_cycles > 0)
            ? total_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6 : 0.0;
        double max_ms = max_cycles * NS_PER_CYCLE / 1e6;

        if (N == sweep.front()) baseline_per_bank = avg_per_bank_M;
        double ratio = (baseline_per_bank > 0) ? avg_per_bank_M / baseline_per_bank : 1.0;

        uint32_t threads = total_tiles * 2;
        fmt::print("{:>7d} | {:>8d} | {:>14d} | {:>10.1f} | {:>14.3f} | {:>14.3f} | {:>9.2f}x\n",
                   N, threads, static_cast<int>(total_acts), max_ms,
                   avg_per_bank_M, aggr_M, ratio);
    }

    fmt::print("\nInterpretation:\n");
    fmt::print("  per-bank flat, aggr scales linearly  → channel can serve N banks in parallel (independent banks)\n");
    fmt::print("  per-bank falls 1/N, aggr ~ flat       → channel command bus saturated; banks share serve rate\n");
    fmt::print("  intermediate                          → partial scaling (limited by tFAW/tRRD or command bus depth)\n");
    fmt::print("  per-bank > 28 M baseline              → ROWS_PER_BANK=16 is wrong; rows actually span fewer banks\n");

    mesh->close();
    return 0;
}
