// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Latency-banded ACT-rate validation probe.
//
// Sweeps n_rows ∈ {1, 2, 4, 8, 16} for two configs:
//   1. Single-thread (1 RISC, 1 sub-port): clean baseline, no contention
//   2. Multi-thread (96 threads = 3 sub-ports × 16 tiles × dual-RISC):
//      same config as long_hammer / peak_attack
//
// For each (config, N), kernel records per-read latency histogram. We then
// derive:
//   - hit fraction (latency < 850 cyc): row-buffer hits, no ACT
//   - miss fraction (latency 850-980 cyc): real ACTs, uncontended
//   - contention fraction (latency > 980 cyc): real ACTs, queued behind others
//
// Goal: validate that AGGREGATE real ACT rate at multi-thread N=2 caps at
// the per-bank tRC physical limit (~21 M ACT/s for 48 ns tRC). Direct
// observation of "miss + contention" reads gives the ACT count without
// inferring through coalescing decomposition.

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
static constexpr uint32_t RESULT_WORDS = 24;
static constexpr uint32_t NUM_BINS     = 16;

static constexpr uint32_t BIN_EDGES[NUM_BINS] = {
     400, 425, 450, 475, 500, 525, 550, 600,
     700, 850, 1000, 1300, 2000, 3500, 8000, 0xFFFFFFFFu
};

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

struct Trial {
    uint32_t n_rows;
    uint32_t total_reads;
    uint32_t total_niu_req;
    uint64_t max_cycles;
    uint64_t bins[NUM_BINS];
    uint32_t min_lat_overall;
    uint32_t max_lat_overall;
};

static void print_trial_row(const Trial& t, double sec, uint32_t threads) {
    double aggr_M = sec > 0 ? t.total_reads / sec / 1e6 : 0;
    double per_thr = aggr_M / threads;

    // Hit zone: bins 0..3 (< 475 cyc) - row-buffer hits, no ACT
    // Miss zone: bins 4..7 (475–600 cyc) - real ACT, ~40 cyc above hits
    // Contention zone: bins 8..15 (≥ 600 cyc) - queued behind other threads' ACTs
    uint64_t hits = t.bins[0] + t.bins[1] + t.bins[2] + t.bins[3];
    uint64_t miss = t.bins[4] + t.bins[5] + t.bins[6] + t.bins[7];
    uint64_t cont = 0;
    for (uint32_t i = 8; i < NUM_BINS; i++) cont += t.bins[i];
    uint64_t total = hits + miss + cont;
    double hit_pct  = total ? 100.0 * hits / total : 0;
    double miss_pct = total ? 100.0 * miss / total : 0;
    double cont_pct = total ? 100.0 * cont / total : 0;

    // ACT-confirmed reads (any latency >= 850 cyc indicates ACT happened)
    uint64_t act_reads = miss + cont;
    double act_M = sec > 0 ? act_reads / sec / 1e6 : 0;
    double niu_ratio = t.total_reads ? double(t.total_niu_req) / double(t.total_reads) : 0;

    fmt::print(
      "{:>3}  {:>10.2f}  {:>9.3f}  {:>5.1f}% {:>5.1f}% {:>5.1f}%  {:>9.2f}  {:>5.0f}/{:<5}  {:>6.3f}\n",
      t.n_rows, aggr_M, per_thr, hit_pct, miss_pct, cont_pct, act_M,
      double(t.min_lat_overall), double(t.max_lat_overall), niu_ratio);
}

static void print_histogram(const Trial& t) {
    uint64_t total = 0;
    for (uint32_t b = 0; b < NUM_BINS; b++) total += t.bins[b];
    if (!total) return;
    fmt::print("    Histogram (N={}, peak bins shown):\n", t.n_rows);
    uint32_t shown = 0;
    for (uint32_t b = 0; b < NUM_BINS && shown < 6; b++) {
        if (t.bins[b] == 0) continue;
        uint32_t lo = (b == 0) ? 0 : BIN_EDGES[b-1];
        uint32_t hi = BIN_EDGES[b];
        double pct = 100.0 * t.bins[b] / total;
        fmt::print("      {:>5} .. {:<6} cyc : {:>10} ({:>5.1f}%)\n",
                   lo, hi == 0xFFFFFFFFu ? 99999 : hi, t.bins[b], pct);
        shown++;
    }
}

int main(int argc, char** argv) {
    uint32_t channel_idx     = 0;
    uint32_t base_row        = 30000;
    uint32_t reads_per_thread_st = 4000;
    uint32_t reads_per_thread_mt = 4000;
    bool     skip_single = false;
    bool     skip_multi  = false;

    for (int i = 1; i < argc; i++) {
        if      (!std::strcmp(argv[i], "--channel") && i+1 < argc) channel_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--base-row")&& i+1 < argc) base_row    = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reads-st")&& i+1 < argc) reads_per_thread_st = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reads-mt")&& i+1 < argc) reads_per_thread_mt = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--skip-single")) skip_single = true;
        else if (!std::strcmp(argv[i], "--skip-multi"))  skip_multi  = true;
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--base-row N] [--reads-st N] [--reads-mt N]\n"
                       "              [--skip-single] [--skip-multi]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) return 1;
    const auto& ch = CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];
    auto workers = compute_grid_workers(mesh.get());

    uint32_t l1_base   = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t result_size = RESULT_WORDS * sizeof(uint32_t);
    uint32_t b_scratch = (l1_base   + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t b_result  = (b_scratch + CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_scratch = (b_result  + result_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_result  = (n_scratch + CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (base_row < min_safe_row) base_row = min_safe_row;
    uint32_t base_addr = base_row * ROW_SIZE;

    static const uint32_t N_VALUES[] = {1, 2, 4, 8, 16};

    auto run_trial = [&](const std::vector<CoreCoord>& sel, uint32_t threads,
                         bool dual_risc, uint32_t reads_per_thread) -> std::vector<Trial>
    {
        std::vector<Trial> results;
        for (uint32_t n_rows : N_VALUES) {
            std::set<CoreRange> ranges;
            for (auto& c : sel) ranges.insert(CoreRange(c, c));
            CoreRangeSet core_set(ranges);

            Program prog = CreateProgram();
            KernelHandle b_kid = CreateKernel(
                prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/latency_band_kernel.cpp",
                core_set,
                DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                                    .noc       = NOC::RISCV_0_default });
            KernelHandle n_kid = 0;
            if (dual_risc) {
                n_kid = CreateKernel(
                    prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/latency_band_kernel.cpp",
                    core_set,
                    DataMovementConfig{ .processor = DataMovementProcessor::RISCV_1,
                                        .noc       = NOC::RISCV_1_default });
            }

            // Distribute tiles across the 3 sub-ports of the channel.
            for (size_t gi = 0; gi < sel.size(); gi++) {
                uint32_t sp_idx = (gi * 3) / sel.size();  // even split across 3 sub-ports
                if (sp_idx > 2) sp_idx = 2;
                const auto& sp = ch.sp[sp_idx];
                std::vector<uint32_t> args = {
                    sp.noc_x, sp.noc_y, base_addr, reads_per_thread, n_rows,
                    b_scratch, b_result
                };
                SetRuntimeArgs(prog, b_kid, sel[gi], args);
                if (dual_risc) {
                    std::vector<uint32_t> n_args = {
                        sp.noc_x, sp.noc_y, base_addr, reads_per_thread, n_rows,
                        n_scratch, n_result
                    };
                    SetRuntimeArgs(prog, n_kid, sel[gi], n_args);
                }
            }

            distributed::MeshWorkload wl;
            wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
            distributed::EnqueueMeshWorkload(cq, wl, false);
            distributed::Finish(cq);

            Trial t{}; t.n_rows = n_rows; t.min_lat_overall = 0xFFFFFFFFu;
            for (size_t gi = 0; gi < sel.size(); gi++) {
                std::vector<uint32_t> rb;
                detail::ReadFromDeviceL1(dev, sel[gi], b_result, result_size, rb);
                t.total_reads   += rb[16];
                t.total_niu_req += rb[19];
                uint64_t cyc = (static_cast<uint64_t>(rb[18]) << 32) | rb[17];
                if (cyc > t.max_cycles) t.max_cycles = cyc;
                for (uint32_t b = 0; b < NUM_BINS; b++) t.bins[b] += rb[b];
                if (rb[21] && rb[21] < t.min_lat_overall) t.min_lat_overall = rb[21];
                if (rb[22] > t.max_lat_overall) t.max_lat_overall = rb[22];

                if (dual_risc) {
                    std::vector<uint32_t> rn;
                    detail::ReadFromDeviceL1(dev, sel[gi], n_result, result_size, rn);
                    t.total_reads   += rn[16];
                    t.total_niu_req += rn[19];
                    uint64_t cyc2 = (static_cast<uint64_t>(rn[18]) << 32) | rn[17];
                    if (cyc2 > t.max_cycles) t.max_cycles = cyc2;
                    for (uint32_t b = 0; b < NUM_BINS; b++) t.bins[b] += rn[b];
                    if (rn[21] && rn[21] < t.min_lat_overall) t.min_lat_overall = rn[21];
                    if (rn[22] > t.max_lat_overall) t.max_lat_overall = rn[22];
                }
            }
            if (t.min_lat_overall == 0xFFFFFFFFu) t.min_lat_overall = 0;
            results.push_back(t);
        }
        return results;
    };

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Latency-Banded ACT Probe — validating tRC bound\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:           {} \n", ch.name);
    fmt::print("║  Base row:          {} (0x{:08x})\n", base_row, base_addr);
    fmt::print("║  Hit  zone (no ACT):  latency < 850 cyc  (~833 same-row)\n");
    fmt::print("║  Miss zone (1 ACT):   850-980 cyc  (~873 diff-row, ~897 cross-bank)\n");
    fmt::print("║  Cont zone (queued):  ≥ 980 cyc  (multi-thread queueing)\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    std::vector<Trial> single_thread, multi_thread;

    if (!skip_single) {
        fmt::print("=== Single-thread (1 RISC, sub-port 0, queue depth = 1) ===\n");
        std::vector<CoreCoord> sel = { workers[0] };
        single_thread = run_trial(sel, 1, false, reads_per_thread_st);
        fmt::print("\n  N    M reads/s   per-thr   hits% miss% cont%   M ACT/s   min/max     NIU:rd\n");
        for (auto& t : single_thread) {
            double sec = t.max_cycles * NS_PER_CYCLE / 1e9;
            print_trial_row(t, sec, 1);
        }
        fmt::print("\n");
        for (auto& t : single_thread) print_histogram(t);
        fmt::print("\n");
    }

    if (!skip_multi) {
        uint32_t tiles_per_sp = 16;
        uint32_t need_tiles = 3 * tiles_per_sp;
        if (workers.size() < need_tiles) {
            fmt::print(stderr, "ERROR: need {} tiles\n", need_tiles); return 1;
        }
        fmt::print("=== Multi-thread (3 sub-ports × {} tiles × 2 RISCs = {} threads, queue depth = {}) ===\n",
                   tiles_per_sp, need_tiles * 2, need_tiles * 2);
        std::vector<CoreCoord> sel(workers.begin(), workers.begin() + need_tiles);
        multi_thread = run_trial(sel, need_tiles * 2, true, reads_per_thread_mt);
        fmt::print("\n  N    M reads/s   per-thr   hits% miss% cont%   M ACT/s   min/max     NIU:rd\n");
        for (auto& t : multi_thread) {
            double sec = t.max_cycles * NS_PER_CYCLE / 1e9;
            print_trial_row(t, sec, need_tiles * 2);
        }
        fmt::print("\n");
        for (auto& t : multi_thread) print_histogram(t);
    }

    fmt::print("\n[Validation of tRC bound]\n");
    fmt::print("  At N=2 (alternating rows in same bank), every read MUST cause one ACT\n");
    fmt::print("  unless the controller reorders.\n");
    fmt::print("  - Single-thread: queue depth=1, no reorder possible. Expect ~100%% miss/cont.\n");
    fmt::print("    Aggregate ACT rate = read rate. tRC bound: ~21 M/s (48 ns).\n");
    fmt::print("  - Multi-thread:  queue depth=192 reads in flight. If controller reorders,\n");
    fmt::print("    miss%% drops below 100%% and aggregate ACT rate (M ACT/s column) is what counts.\n");
    fmt::print("    M ACT/s should cap at ~21 M (per-bank tRC) regardless of how high reads/s goes.\n");

    mesh->close();
    return 0;
}
