// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Long-run pipelined hammer attack.
//
// Single-bank peak config (1 channel × 3 sub-ports × 16 tiles × dual-RISC,
// 96 threads) hammering 2 same-bank aggressor rows pipelined K=16. Time-
// bounded by --duration-min flag; intended for 1-3 hour SSH-detached runs
// via nohup.
//
// At pipelined per-thread rate ~10 M reads/s and 96-thread aggregate, we
// expect ~10× the per-row real-ACT rate of barriered peak_attack. Designed
// to definitively rule out user-space rowhammer if no flips emerge.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
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
static constexpr double   BRISC_HZ          = 800e6;
static constexpr uint32_t ROWS_PER_BANK     = 16;
static constexpr uint32_t L1_ALIGN          = 64;

static constexpr uint32_t RESULT_HDR_WORDS  = 12;
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

static std::string now_iso() {
    char buf[64];
    auto t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

int main(int argc, char** argv) {
    uint32_t channel_idx  = 0;
    uint32_t tiles_per_sp = 16;
    uint32_t start_row    = 30000;
    uint32_t num_aggr     = 2;          // V-1, V+1 (classical 2-sided)
    uint32_t data_pattern = 0x55555555;
    double   duration_min = 1.0;        // default 1 minute (smoke)

    for (int i = 1; i < argc; i++) {
        if      (!std::strcmp(argv[i], "--channel")       && i+1 < argc) channel_idx  = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiles-per-sp")  && i+1 < argc) tiles_per_sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--start-row")     && i+1 < argc) start_row    = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--num-aggressors")&& i+1 < argc) num_aggr     = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pattern")       && i+1 < argc) data_pattern = std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--duration-min")  && i+1 < argc) duration_min = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--tiles-per-sp N] [--start-row N]\n"
                       "              [--num-aggressors N] [--pattern HEX] [--duration-min FLOAT]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) return 1;

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
    const auto& ch = CHANNELS[channel_idx];

    uint32_t l1_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t result_size = RESULT_BUF_WORDS * sizeof(uint32_t);
    // Layout per tile: BRISC scratch (16 CL) + BRISC result, then NCRISC scratch + NCRISC result
    uint32_t b_scratch = (l1_base   + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t b_result  = (b_scratch + 16 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_scratch = (b_result  + result_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_result  = (n_scratch + 16 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (start_row < min_safe_row) start_row = min_safe_row;
    uint32_t bank_start = (start_row / ROWS_PER_BANK) * ROWS_PER_BANK;
    uint32_t victim_row = bank_start + ROWS_PER_BANK / 2;
    uint32_t victim_addr = victim_row * ROW_SIZE;

    std::vector<uint32_t> aggr_addrs;
    for (uint32_t d = 1; aggr_addrs.size() < num_aggr && d < ROWS_PER_BANK; d++) {
        if (victim_row >= d + bank_start)
            aggr_addrs.push_back((victim_row - d) * ROW_SIZE);
        if (aggr_addrs.size() < num_aggr && victim_row + d <= bank_start + ROWS_PER_BANK - 1)
            aggr_addrs.push_back((victim_row + d) * ROW_SIZE);
    }
    if (aggr_addrs.size() < num_aggr) num_aggr = aggr_addrs.size();

    uint64_t duration_cycles = static_cast<uint64_t>(duration_min * 60.0 * BRISC_HZ);

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       LONG-RUN PIPELINED HAMMER\n");
    fmt::print("║       started: {}\n", now_iso());
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:           {} all 3 sub-ports\n", ch.name);
    fmt::print("║  Tiles/sub-port:    {} (× 2 RISCs = {} threads)\n", tiles_per_sp, 3 * tiles_per_sp * 2);
    fmt::print("║  Victim row:        {} (0x{:08x})\n", victim_row, victim_addr);
    fmt::print("║  Aggressors ({:>2}):\n", num_aggr);
    for (uint32_t i = 0; i < num_aggr; i++)
        fmt::print("║    [{}] row {} (0x{:08x})\n", i, aggr_addrs[i] / ROW_SIZE, aggr_addrs[i]);
    fmt::print("║  Pattern:           0x{:08x}\n", data_pattern);
    fmt::print("║  Duration:          {:.2f} min  ({} cycles)\n", duration_min, duration_cycles);
    fmt::print("║  Pipelined K=16, no per-read barrier (DRAM-bound)\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");
    fflush(stdout);

    fmt::print("[Step 1] Pre-attack ECC counters\n");
    EccCounters ecc_pre = read_ecc();
    if (ecc_pre.valid)
        fmt::print("  corr ch01:{} ch23:{} ch45:{} ch67:{}  uncorr:{}\n",
                   ecc_pre.corr[0], ecc_pre.corr[1], ecc_pre.corr[2], ecc_pre.corr[3], ecc_pre.uncorr);
    else
        fmt::print("  WARNING: pyluwen unavailable; ECC will not be reported\n");

    fmt::print("\n[Step 2] Launching pipelined hammer for {:.2f} min ({:.1f} hours)\n",
               duration_min, duration_min / 60.0);
    fmt::print("  Started at: {}\n", now_iso());
    fflush(stdout);

    std::vector<CoreCoord> sel(workers.begin(), workers.begin() + need_tiles);
    std::set<CoreRange> ranges;
    for (auto& c : sel) ranges.insert(CoreRange(c, c));
    CoreRangeSet core_set(ranges);

    Program prog = CreateProgram();
    KernelHandle b_kid = CreateKernel(
        prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/long_hammer_kernel.cpp",
        core_set,
        DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                            .noc       = NOC::RISCV_0_default });
    KernelHandle n_kid = CreateKernel(
        prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/long_hammer_kernel.cpp",
        core_set,
        DataMovementConfig{ .processor = DataMovementProcessor::RISCV_1,
                            .noc       = NOC::RISCV_1_default });

    uint32_t dur_lo = static_cast<uint32_t>(duration_cycles);
    uint32_t dur_hi = static_cast<uint32_t>(duration_cycles >> 32);

    for (uint32_t s = 0; s < 3; s++) {
        const auto& sp = ch.sp[s];
        for (uint32_t i = 0; i < tiles_per_sp; i++) {
            uint32_t global_tile = s * tiles_per_sp + i;
            bool is_primary = (s == 0 && i == 0);
            uint32_t b_core_id = is_primary ? 0 : (global_tile * 2 + 100);
            uint32_t n_core_id = global_tile * 2 + 101;

            std::vector<uint32_t> b_args = {
                sp.noc_x, sp.noc_y, victim_addr,
                dur_lo, dur_hi, data_pattern,
                b_scratch, b_result, b_core_id,
                num_aggr,
            };
            for (auto a : aggr_addrs) b_args.push_back(a);
            SetRuntimeArgs(prog, b_kid, sel[global_tile], b_args);

            std::vector<uint32_t> n_args = {
                sp.noc_x, sp.noc_y, victim_addr,
                dur_lo, dur_hi, data_pattern,
                n_scratch, n_result, n_core_id,
                num_aggr,
            };
            for (auto a : aggr_addrs) n_args.push_back(a);
            SetRuntimeArgs(prog, n_kid, sel[global_tile], n_args);
        }
    }

    auto t_host_start = std::chrono::steady_clock::now();
    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);
    auto t_host_end = std::chrono::steady_clock::now();
    double host_sec = std::chrono::duration<double>(t_host_end - t_host_start).count();
    fmt::print("  Hammer complete at: {} ({:.1f} min wall)\n",
               now_iso(), host_sec / 60.0);
    fflush(stdout);

    fmt::print("\n[Step 3] Post-attack ECC counters\n");
    EccCounters ecc_post = read_ecc();
    if (ecc_post.valid)
        fmt::print("  corr ch01:{} ch23:{} ch45:{} ch67:{}  uncorr:{}\n",
                   ecc_post.corr[0], ecc_post.corr[1], ecc_post.corr[2], ecc_post.corr[3], ecc_post.uncorr);
    if (ecc_pre.valid && ecc_post.valid) {
        uint32_t d_tot = ecc_post.total_corr() - ecc_pre.total_corr();
        uint32_t d_unc = ecc_post.uncorr - ecc_pre.uncorr;
        if (d_tot > 0 || d_unc > 0)
            fmt::print("  *** ECC delta: +{} corrected, +{} uncorrectable ***\n", d_tot, d_unc);
        else
            fmt::print("  ECC delta: NO new errors\n");
    }

    fmt::print("\n[Step 4] Aggregate stats + per-bank verification\n");
    uint64_t total_acts = 0, max_cycles = 0;
    uint64_t total_niu = 0;
    uint32_t kernel_flips_cl = 0;
    uint32_t kernel_flips_bits = 0;
    std::vector<uint32_t> primary_buf;

    for (uint32_t s = 0; s < 3; s++) {
        for (uint32_t i = 0; i < tiles_per_sp; i++) {
            uint32_t gi = s * tiles_per_sp + i;
            std::vector<uint32_t> rb, rn;
            detail::ReadFromDeviceL1(dev, sel[gi], b_result, result_size, rb);
            detail::ReadFromDeviceL1(dev, sel[gi], n_result, result_size, rn);
            uint64_t b_acts = (static_cast<uint64_t>(rb[3]) << 32) | rb[2];
            uint64_t n_acts = (static_cast<uint64_t>(rn[3]) << 32) | rn[2];
            uint64_t b_cyc  = (static_cast<uint64_t>(rb[5]) << 32) | rb[4];
            uint64_t n_cyc  = (static_cast<uint64_t>(rn[5]) << 32) | rn[4];
            total_acts  += b_acts + n_acts;
            total_niu   += rb[6] + rn[6];
            if (b_cyc > max_cycles) max_cycles = b_cyc;
            if (n_cyc > max_cycles) max_cycles = n_cyc;
            if (s == 0 && i == 0) {
                kernel_flips_cl   = rb[8];
                kernel_flips_bits = rb[9];
                primary_buf = rb;
            }
        }
    }
    double sec = max_cycles * NS_PER_CYCLE / 1e9;
    double aggr_M = sec > 0 ? total_acts / sec / 1e6 : 0;

    fmt::print("  Total kernel reads issued (NIU MST): {} (~{:.2f}B)\n", total_niu, total_niu / 1e9);
    fmt::print("  Total reads counted by kernel:       {} (~{:.2f}B)\n", total_acts, total_acts / 1e9);
    fmt::print("  Max wall time:                       {:.2f} min\n", sec / 60.0);
    fmt::print("  Aggregate channel rate:              {:.2f} M reads/s\n", aggr_M);
    fmt::print("  Per-row rate (assuming 2 aggressors):{:.2f} M reads/s/row\n", aggr_M / num_aggr);
    fmt::print("\n  --- VICTIM ROW VERIFICATION (primary thread) ---\n");
    fmt::print("  Flipped cachelines: {}\n", kernel_flips_cl);
    fmt::print("  Total bit flips:    {}\n", kernel_flips_bits);
    if (kernel_flips_cl > 0) {
        fmt::print("\n  *** FLIPS DETECTED ***\n");
        uint32_t n = std::min(kernel_flips_cl, MAX_FLIP_RECORDS);
        for (uint32_t r = 0; r < n; r++) {
            uint32_t base = RESULT_HDR_WORDS + r * FLIP_RECORD_WORDS;
            fmt::print("    flip CL{} word{}: expected 0x{:08x} got 0x{:08x}\n",
                       primary_buf[base], primary_buf[base+1],
                       primary_buf[base+2], primary_buf[base+3]);
        }
    } else {
        fmt::print("  No flips detected.\n");
    }
    fmt::print("\n  Run complete: {}\n", now_iso());

    mesh->close();
    return kernel_flips_cl > 0 ? 0 : 0;
}
