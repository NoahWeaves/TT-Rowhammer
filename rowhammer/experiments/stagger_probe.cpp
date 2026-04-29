// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Thread-stagger sweep — measures the controller's temporal reorder window.
//
// 96 threads (3 sub-ports × 16 tiles × dual-RISC) on ch0, all hammering V±1
// alternating (n_rows=2). Each thread has start_delay = thread_idx ×
// stagger_step cycles. We sweep stagger_step.
//
// At stagger=0: all threads start nearly simultaneously, deep instantaneous
// queue at the controller, maximum reorder/coalescing opportunity.
//
// At stagger=large: threads decoupled in time, each one's K=16 burst
// finishes before the next thread's begins, controller queue is shallow,
// minimal coalescing.
//
// We expect:
//   - stagger=0: high read rate, low miss%, low ACT/s
//   - stagger=large: read rate drops (per-thread NOC overhead now visible),
//                    miss% rises toward 100%, ACT/s asymptotes at tRC bound
//
// The stagger value where miss% reaches its plateau IS the controller's
// effective reorder time-window depth.

#include <algorithm>
#include <array>
#include <chrono>
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

// Bin edges (must match stagger_probe_kernel.cpp): 400, 425, 450, 475, 500,
// 525, 550, 600, 700, 850, 1000, 1300, 2000, 3500, 8000, ∞.

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
    uint32_t channel_idx     = 0;
    uint32_t base_row        = 30000;
    uint32_t reads_per_thread = 4000;
    uint32_t n_rows          = 2;       // V, V+1 alternating
    uint32_t tiles_per_sp    = 16;
    bool     deep            = false;   // deep-stagger sweep (forces decoupling)

    for (int i = 1; i < argc; i++) {
        if      (!std::strcmp(argv[i], "--channel")  && i+1 < argc) channel_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--base-row") && i+1 < argc) base_row    = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reads")    && i+1 < argc) reads_per_thread = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--n-rows")   && i+1 < argc) n_rows      = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiles-per-sp") && i+1 < argc) tiles_per_sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--deep")) deep = true;
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--base-row N] [--reads N] [--n-rows N] [--tiles-per-sp N] [--deep]\n"
                       "  --deep: short loops + large staggers, forces thread decoupling.\n", argv[0]);
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

    uint32_t need_tiles = 3 * tiles_per_sp;
    if (workers.size() < need_tiles) {
        fmt::print(stderr, "ERROR: need {} tiles\n", need_tiles); return 1;
    }
    std::vector<CoreCoord> sel(workers.begin(), workers.begin() + need_tiles);

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

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Thread-Stagger Probe — controller reorder window\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:           {} all 3 sub-ports\n", ch.name);
    fmt::print("║  Threads:           {} (3 sp × {} tiles × 2 RISCs)\n", need_tiles*2, tiles_per_sp);
    fmt::print("║  Base row:          {} (0x{:08x})\n", base_row, base_addr);
    fmt::print("║  Pattern:           N={} alternating in same bank\n", n_rows);
    fmt::print("║  Reads/thread:      {}\n", reads_per_thread);
    fmt::print("║  Stagger semantics: thread_id × stagger_step cycles delay before timed loop\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    // Stagger steps to sweep, in BRISC cycles (~1.25 ns each at 800 MHz reference)
    static const uint32_t STAGGER_VALUES_NORMAL[] = {
        0, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000
    };
    static const uint32_t STAGGER_VALUES_DEEP[] = {
        0, 1000, 5000, 25000, 100000, 250000, 500000, 1000000, 2000000
    };
    if (deep) reads_per_thread = std::min(reads_per_thread, 200u);  // short loops
    const uint32_t* stagger_arr = deep ? STAGGER_VALUES_DEEP : STAGGER_VALUES_NORMAL;
    size_t stagger_count = deep ? (sizeof(STAGGER_VALUES_DEEP)/sizeof(uint32_t))
                                : (sizeof(STAGGER_VALUES_NORMAL)/sizeof(uint32_t));

    // Estimate per-thread loop length to flag whether stagger truly decouples threads.
    uint32_t per_thread_cyc_est = reads_per_thread * 440u;  // ~440 cyc/read serialized
    uint32_t num_threads = need_tiles * 2;

    fmt::print("Per-thread loop est: {} reads × ~440 cyc = {} cyc (~{:.1f} µs)\n",
               reads_per_thread, per_thread_cyc_est,
               per_thread_cyc_est * NS_PER_CYCLE / 1000.0);
    fmt::print("Decoupling threshold: stagger > per_loop / num_threads = {} cyc\n\n",
               per_thread_cyc_est / num_threads);

    fmt::print("{:>9} {:>9} {:>11} {:>10} {:>9} {:>7} {:>7} {:>9}\n",
               "stagger", "wall ms", "M reads/s", "(per-thr)", "NIU:rd",
               "hit%", "miss%", "M ACT/s");
    fmt::print("{:-<85}\n", "");

    for (size_t si = 0; si < stagger_count; si++) {
        uint32_t stagger_step = stagger_arr[si];
        std::set<CoreRange> ranges;
        for (auto& c : sel) ranges.insert(CoreRange(c, c));
        CoreRangeSet core_set(ranges);

        Program prog = CreateProgram();
        KernelHandle b_kid = CreateKernel(
            prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/stagger_probe_kernel.cpp",
            core_set,
            DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                                .noc       = NOC::RISCV_0_default });
        KernelHandle n_kid = CreateKernel(
            prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/stagger_probe_kernel.cpp",
            core_set,
            DataMovementConfig{ .processor = DataMovementProcessor::RISCV_1,
                                .noc       = NOC::RISCV_1_default });

        uint32_t thread_idx = 0;
        for (size_t gi = 0; gi < sel.size(); gi++) {
            uint32_t sp_idx = (gi * 3) / sel.size();
            if (sp_idx > 2) sp_idx = 2;
            const auto& sp = ch.sp[sp_idx];
            uint32_t b_delay = thread_idx * stagger_step; thread_idx++;
            uint32_t n_delay = thread_idx * stagger_step; thread_idx++;
            std::vector<uint32_t> b_args = {
                sp.noc_x, sp.noc_y, base_addr, reads_per_thread, n_rows,
                b_scratch, b_result, b_delay
            };
            std::vector<uint32_t> n_args = {
                sp.noc_x, sp.noc_y, base_addr, reads_per_thread, n_rows,
                n_scratch, n_result, n_delay
            };
            SetRuntimeArgs(prog, b_kid, sel[gi], b_args);
            SetRuntimeArgs(prog, n_kid, sel[gi], n_args);
        }

        distributed::MeshWorkload wl;
        wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
        auto host_t0 = std::chrono::steady_clock::now();
        distributed::EnqueueMeshWorkload(cq, wl, false);
        distributed::Finish(cq);
        auto host_t1 = std::chrono::steady_clock::now();
        double host_sec = std::chrono::duration<double>(host_t1 - host_t0).count();

        uint64_t total_reads = 0, total_niu = 0, max_cyc = 0, min_cyc = UINT64_MAX;
        uint64_t bins[NUM_BINS] = {0};
        uint32_t mn_lat = 0xFFFFFFFFu, mx_lat = 0;
        for (size_t gi = 0; gi < sel.size(); gi++) {
            std::vector<uint32_t> rb, rn;
            detail::ReadFromDeviceL1(dev, sel[gi], b_result, result_size, rb);
            detail::ReadFromDeviceL1(dev, sel[gi], n_result, result_size, rn);
            total_reads += rb[16] + rn[16];
            total_niu   += rb[19] + rn[19];
            uint64_t bc = (uint64_t(rb[18])<<32) | rb[17];
            uint64_t nc = (uint64_t(rn[18])<<32) | rn[17];
            if (bc > max_cyc) max_cyc = bc;
            if (nc > max_cyc) max_cyc = nc;
            if (bc > 0 && bc < min_cyc) min_cyc = bc;
            if (nc > 0 && nc < min_cyc) min_cyc = nc;
            for (uint32_t b = 0; b < NUM_BINS; b++) bins[b] += rb[b] + rn[b];
            if (rb[21] && rb[21] < mn_lat) mn_lat = rb[21];
            if (rn[21] && rn[21] < mn_lat) mn_lat = rn[21];
            if (rb[22] > mx_lat) mx_lat = rb[22];
            if (rn[22] > mx_lat) mx_lat = rn[22];
        }
        if (mn_lat == 0xFFFFFFFFu) mn_lat = 0;
        if (min_cyc == UINT64_MAX) min_cyc = 0;
        double sec = max_cyc * NS_PER_CYCLE / 1e9;
        // True aggregate uses host wall-clock (captures stagger spread)
        double aggr_M = host_sec > 0 ? total_reads / host_sec / 1e6 : 0;
        double aggr_M_perthr = sec > 0 ? total_reads / sec / 1e6 : 0;  // old metric
        double niu_ratio = total_reads ? double(total_niu)/double(total_reads) : 0;
        (void)mn_lat; (void)mx_lat; (void)min_cyc;
        // niu_M not printed in deep mode header; kept above for reference
        (void)total_niu;

        // Hit cluster: bin 1 ([425,450)); Miss cluster: bin 2 ([450,475))
        // Lower bin 0 (<425) is NOC-distance artifact; higher bins are contention/refresh.
        uint64_t hits = bins[1];
        uint64_t miss = bins[2];
        uint64_t cont = 0; for (uint32_t i=3;i<NUM_BINS;i++) cont += bins[i];
        uint64_t artifact = bins[0];
        uint64_t total = hits + miss + cont + artifact;
        double hit_pct  = total ? 100.0*hits/total : 0;
        double miss_pct = total ? 100.0*miss/total : 0;
        // Implied real ACT rate uses miss+cont fraction (everything ≥450 cyc)
        // applied to host-wall aggregate read rate.
        uint64_t act_reads = miss + cont;
        double act_M = host_sec > 0 ? act_reads / host_sec / 1e6 : 0;

        fmt::print("{:>9} {:>9.1f} {:>11.2f} {:>10.2f} {:>9.3f} {:>6.1f}% {:>6.1f}% {:>9.2f}\n",
                   stagger_step, host_sec * 1000.0,
                   aggr_M, aggr_M_perthr, niu_ratio,
                   hit_pct, miss_pct, act_M);
    }

    fmt::print("\n[Interpretation]\n");
    fmt::print("  Two read-rate columns:\n");
    fmt::print("    M reads/s    — TRUE aggregate (host wall time, captures stagger spread)\n");
    fmt::print("    (per-thr)    — per-thread amortized × 96 (constant if loops same length)\n");
    fmt::print("  As stagger grows beyond decoupling threshold, M reads/s should DROP because\n");
    fmt::print("  threads no longer overlap. miss%% should RISE if coalescing weakens with\n");
    fmt::print("  reduced overlap. M ACT/s = (miss%% + cont%%) × M reads/s — plateau at ~24 M\n");
    fmt::print("  if tRC binds; rising past 24 M would invalidate our per-bank ceiling claim.\n");

    mesh->close();
    return 0;
}
