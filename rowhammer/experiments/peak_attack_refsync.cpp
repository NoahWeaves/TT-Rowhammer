// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Peak-config rowhammer attack with REF-sync delay sweep.
//
// Uses the FLUSH-READ + delay kernel (core_scaling_refsync_kernel.cpp) at
// the empirical peak per-row config:
//   - 1 GDDR6 channel × 3 NOC sub-ports × 16 tiles dual-RISC = 96 threads
//   - num_sides = 2 (classic double-sided, max per-row activation rate)
//
// Sweeps `delay_iters` (per-aggressor-sweep spin count, ~1 BRISC cycle each)
// to test whether shifting activations relative to refresh windows induces
// any flips or ECC corrections.
//
// At each delay value: write pattern, hammer for `iterations` sweeps,
// readback victim, count flips, read ECC delta.

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

static constexpr uint32_t ROW_SIZE          = 8192;
static constexpr uint32_t CACHELINE         = 64;
static constexpr double   NS_PER_CYCLE      = 1.25;
static constexpr uint32_t ROWS_PER_BANK     = 16;
static constexpr uint32_t L1_ALIGN          = 64;

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

// ECC counter monitoring (matches rowhammer_nsided.cpp)
struct EccCounters {
    uint32_t corr[4]; uint32_t uncorr; bool valid;
    uint32_t total_corr() const { return corr[0]+corr[1]+corr[2]+corr[3]; }
};
static EccCounters read_ecc() {
    EccCounters ec{}; ec.valid = false;
    const char* cmd =
        "python3 -c \"from pyluwen import PciChip; "
        "t=PciChip(pci_interface=0).get_telemetry(); "
        "print(t.gddr01_corr_errs, t.gddr23_corr_errs, "
              "t.gddr45_corr_errs, t.gddr67_corr_errs, t.gddr_uncorr_errs)"
        "\" 2>/dev/null";
    FILE* p = popen(cmd, "r"); if (!p) return ec;
    char buf[256];
    if (fgets(buf, sizeof(buf), p)) {
        if (sscanf(buf, "%u %u %u %u %u", &ec.corr[0], &ec.corr[1],
                   &ec.corr[2], &ec.corr[3], &ec.uncorr) == 5) ec.valid = true;
    }
    pclose(p);
    return ec;
}

static std::vector<CoreCoord> compute_grid_workers(distributed::MeshDevice* mesh) {
    CoreCoord g = mesh->compute_with_storage_grid_size();
    std::vector<CoreCoord> out; out.reserve(g.x * g.y);
    for (uint32_t y = 0; y < g.y; y++)
        for (uint32_t x = 0; x < g.x; x++) out.push_back({x, y});
    return out;
}

int main(int argc, char** argv) {
    uint32_t channel_idx  = 0;
    uint32_t tiles_per_sp = 16;
    uint32_t start_row    = 2000;
    uint32_t num_sides    = 2;             // classic double-sided
    uint32_t hammer_iters = 1000000;       // 1M sweeps × 2 aggressors = 2M acts/thread
    uint32_t data_pattern = 0x55555555;
    std::vector<uint32_t> delay_sweep = {0, 8, 16, 32, 48, 64, 100, 200, 500};

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--channel") && i+1 < argc)        channel_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiles-per-sp") && i+1 < argc) tiles_per_sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--start-row") && i+1 < argc) start_row = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--num-sides") && i+1 < argc) num_sides = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations") && i+1 < argc) hammer_iters = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pattern") && i+1 < argc) data_pattern = std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--delays") && i+1 < argc) {
            delay_sweep.clear();
            const char* s = argv[++i];
            while (*s) {
                char* e; uint32_t v = std::strtoul(s, &e, 10);
                if (e == s) break; delay_sweep.push_back(v); s = e;
                if (*s == ',') s++;
            }
        }
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--tiles-per-sp N] [--start-row N]\n"
                       "              [--num-sides N] [--iterations N] [--pattern HEX]\n"
                       "              [--delays D1,D2,D3,...]\n", argv[0]);
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
    if (workers.size() < need_tiles) return 1;

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
    std::vector<uint32_t> aggrs;
    for (uint32_t d = 1; aggrs.size() < num_sides && d < ROWS_PER_BANK; d++) {
        if (victim_row >= d + bank_start) aggrs.push_back((victim_row - d) * ROW_SIZE);
        if (aggrs.size() < num_sides && victim_row + d <= bank_start + ROWS_PER_BANK - 1)
            aggrs.push_back((victim_row + d) * ROW_SIZE);
    }

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Peak REF-sync Attack ({}, all 3 sub-ports)             ║\n", ch.name);
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:       {} sub-ports ({},{}) ({},{}) ({},{})\n",
               ch.name, ch.sp[0].noc_x, ch.sp[0].noc_y,
               ch.sp[1].noc_x, ch.sp[1].noc_y, ch.sp[2].noc_x, ch.sp[2].noc_y);
    fmt::print("║  Tiles/sub-port:{} (× 2 RISCs = 32 threads/sp = 96 total)\n", tiles_per_sp);
    fmt::print("║  Victim row:    {} (0x{:08x})\n", victim_row, victim_addr);
    fmt::print("║  Aggressors:    {}", aggrs.size());
    for (auto a : aggrs) fmt::print(" {}", a / ROW_SIZE);
    fmt::print("\n");
    fmt::print("║  Iterations:    {} sweeps/thread = {} acts/thread\n",
               hammer_iters, hammer_iters * num_sides);
    fmt::print("║  Pattern:       0x{:08x}\n", data_pattern);
    fmt::print("║  Delays:       ");
    for (auto d : delay_sweep) fmt::print(" {}", d);
    fmt::print("\n╚══════════════════════════════════════════════════════════════╝\n\n");

    fmt::print("{:>6s} | {:>14s} | {:>10s} | {:>14s} | {:>10s} | {:>5s} | {:>10s}\n",
               "delay", "total acts", "max ms", "aggr Mas", "per-row M", "flips", "ECC corr");
    fmt::print("{:->6s}-+-{:->14s}-+-{:->10s}-+-{:->14s}-+-{:->10s}-+-{:->5s}-+-{:->10s}\n",
               "", "", "", "", "", "", "");

    for (uint32_t delay : delay_sweep) {
        std::vector<CoreCoord> sel(workers.begin(), workers.begin() + need_tiles);
        std::set<CoreRange> ranges;
        for (auto& c : sel) ranges.insert(CoreRange(c, c));
        CoreRangeSet core_set(ranges);

        Program prog = CreateProgram();
        KernelHandle b_kid = CreateKernel(
            prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/core_scaling_refsync_kernel.cpp",
            core_set,
            DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                                .noc       = NOC::RISCV_0_default });
        KernelHandle n_kid = CreateKernel(
            prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/core_scaling_refsync_kernel.cpp",
            core_set,
            DataMovementConfig{ .processor = DataMovementProcessor::RISCV_1,
                                .noc       = NOC::RISCV_1_default });

        for (uint32_t s = 0; s < 3; s++) {
            const auto& sp = ch.sp[s];
            for (uint32_t i = 0; i < tiles_per_sp; i++) {
                uint32_t global_tile = s * tiles_per_sp + i;
                bool is_primary = (s == 0 && i == 0);
                uint32_t b_core_id = is_primary ? 0 : (global_tile * 2 + 100);
                uint32_t n_core_id = global_tile * 2 + 101;
                std::vector<uint32_t> b_args = {
                    sp.noc_x, sp.noc_y, victim_addr,
                    hammer_iters, data_pattern,
                    b_scratch, b_result,
                    b_core_id, delay,
                    static_cast<uint32_t>(aggrs.size()),
                };
                for (auto a : aggrs) b_args.push_back(a);
                SetRuntimeArgs(prog, b_kid, sel[global_tile], b_args);

                std::vector<uint32_t> n_args = {
                    sp.noc_x, sp.noc_y, victim_addr,
                    hammer_iters, data_pattern,
                    n_scratch, n_result,
                    n_core_id, delay,
                    static_cast<uint32_t>(aggrs.size()),
                };
                for (auto a : aggrs) n_args.push_back(a);
                SetRuntimeArgs(prog, n_kid, sel[global_tile], n_args);
            }
        }

        EccCounters ecc_pre = read_ecc();

        distributed::MeshWorkload wl;
        wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
        distributed::EnqueueMeshWorkload(cq, wl, false);
        distributed::Finish(cq);

        EccCounters ecc_post = read_ecc();

        // Aggregate
        uint64_t total_acts = 0, max_cycles = 0;
        uint32_t flips = 0;
        for (uint32_t s = 0; s < 3; s++) {
            for (uint32_t i = 0; i < tiles_per_sp; i++) {
                uint32_t gi = s * tiles_per_sp + i;
                std::vector<uint32_t> rb, rn;
                detail::ReadFromDeviceL1(dev, sel[gi], b_result, result_size, rb);
                detail::ReadFromDeviceL1(dev, sel[gi], n_result, result_size, rn);
                total_acts += rb[5] + rn[5];
                uint64_t cyc_b = (static_cast<uint64_t>(rb[4]) << 32) | rb[3];
                uint64_t cyc_n = (static_cast<uint64_t>(rn[4]) << 32) | rn[3];
                if (cyc_b > max_cycles) max_cycles = cyc_b;
                if (cyc_n > max_cycles) max_cycles = cyc_n;
                if (s == 0 && i == 0) flips = rb[1];
            }
        }
        double aggr_M = (max_cycles > 0)
            ? total_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6 : 0.0;
        double per_row_M = aggr_M / num_sides;
        double max_ms = max_cycles * NS_PER_CYCLE / 1e6;
        uint32_t ecc_d = (ecc_pre.valid && ecc_post.valid)
            ? ecc_post.total_corr() - ecc_pre.total_corr() : 0;

        fmt::print("{:>6d} | {:>14d} | {:>10.1f} | {:>14.3f} | {:>10.3f} | {:>5d} | {:>10d}\n",
                   delay, static_cast<int>(total_acts), max_ms,
                   aggr_M, per_row_M, flips, ecc_d);
    }

    fmt::print("\nInterpretation:\n");
    fmt::print("  flips > 0   → REF-sync at this delay broke through TRR/ECC. WRITE IT UP.\n");
    fmt::print("  ECC > 0     → flips happened but were silently corrected; effective TRR bypass.\n");
    fmt::print("  rate dip    → delay value is large enough to lose throughput; not useful for attack.\n");
    fmt::print("  no signal   → TRR + on-die ECC + tRC physics all hold against this attack class.\n");

    mesh->close();
    return 0;
}
