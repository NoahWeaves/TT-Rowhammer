// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Peak-config rowhammer attack.
//
// Runs the FLUSH-READ hammer at the empirically-best user-space configuration
// found in the core/bank scaling sweeps:
//   - 8 GDDR6 channels (banks) targeted simultaneously, each with its own
//     victim row and same-bank aggressor set.
//   - 16 tiles per bank × 2 RISCs/tile = 32 hammer threads per bank.
//   - 256 total hammer threads on 128 of the 130 worker tiles.
//   - Measured aggregate ~221 M real activations/s (FLUSH-READ ground truth).
//
// Each bank gets its own primary thread (per-bank core_id=0) which writes
// the test pattern, hammers, and verifies the victim row. Other threads in
// the bank are hammer-only.
//
// Pre/post inspection: a tiny inspector kernel reads each bank's victim row
// into L1 before pattern-write (raw pre-existing content) and after the
// attack (final state). The host hashes both, dumps a few cachelines, and
// reports any bit-flip records the kernel found.
//
// ECC counters are read via pyluwen telemetry before and after.

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
static constexpr uint32_t CACHELINES_PER_ROW = ROW_SIZE / CACHELINE;
static constexpr double   NS_PER_CYCLE      = 1.25;
static constexpr uint32_t ROWS_PER_BANK     = 16;
static constexpr uint32_t L1_ALIGN          = 64;
static constexpr uint32_t NUM_CHANNELS      = 8;

static constexpr uint32_t RESULT_HDR_WORDS  = 6;
static constexpr uint32_t MAX_FLIP_RECORDS  = 32;
static constexpr uint32_t FLIP_RECORD_WORDS = 4;
static constexpr uint32_t RESULT_BUF_WORDS  = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

// 3 NOC sub-port endpoints per channel (from blackhole_140_arch.yaml). Index 1
// is the middle/legacy endpoint that prior experiments used; --sub-ports=1
// preserves that exact behavior for backwards compatibility.
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
// Order in which sub-ports are engaged when --sub-ports K < 3: K=1 → {1};
// K=2 → {0,1}; K=3 → {0,1,2}. K=1 reproduces prior single-endpoint behavior.
static const std::array<std::array<uint32_t, 3>, 4> SP_ORDER = {{
    {{0, 0, 0}},  // K=0 unused
    {{1, 0, 0}},  // K=1: middle only
    {{0, 1, 0}},  // K=2: ends + middle (sp 0 and 1)
    {{0, 1, 2}},  // K=3: all
}};

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

// ─── ECC counter monitoring (matches rowhammer_nsided.cpp) ────────────
struct EccCounters {
    uint32_t corr[4];
    uint32_t uncorr;
    bool valid;
    uint32_t total_corr() const { return corr[0]+corr[1]+corr[2]+corr[3]; }
};
static EccCounters read_ecc() {
    EccCounters ec{}; ec.valid = false;
    const char* cmd =
        "python3 -c \""
        "from pyluwen import PciChip; "
        "t=PciChip(pci_interface=0).get_telemetry(); "
        "print(t.gddr01_corr_errs, t.gddr23_corr_errs, "
              "t.gddr45_corr_errs, t.gddr67_corr_errs, "
              "t.gddr_uncorr_errs)"
        "\" 2>/dev/null";
    FILE* p = popen(cmd, "r");
    if (!p) return ec;
    char buf[256];
    if (fgets(buf, sizeof(buf), p)) {
        if (sscanf(buf, "%u %u %u %u %u",
                   &ec.corr[0], &ec.corr[1], &ec.corr[2], &ec.corr[3], &ec.uncorr) == 5)
            ec.valid = true;
    }
    pclose(p);
    return ec;
}
static void print_ecc_delta(const EccCounters& a, const EccCounters& b) {
    if (!a.valid || !b.valid) { fmt::print("  ECC counters unavailable\n"); return; }
    uint32_t d[4] = { b.corr[0]-a.corr[0], b.corr[1]-a.corr[1],
                      b.corr[2]-a.corr[2], b.corr[3]-a.corr[3] };
    uint32_t du = b.uncorr - a.uncorr;
    uint32_t tot = d[0]+d[1]+d[2]+d[3];
    if (tot > 0 || du > 0) {
        fmt::print("  *** ECC delta: +{} corrected (ch01:{} ch23:{} ch45:{} ch67:{}), +{} uncorrectable\n",
                   tot, d[0], d[1], d[2], d[3], du);
        if (tot > 0) fmt::print("    -> ECC is silently correcting bit flips.\n");
        if (du > 0)  fmt::print("    -> UNCORRECTABLE multi-bit errors!\n");
    } else {
        fmt::print("  ECC delta: no new errors\n");
    }
}

// ─── compute-grid worker enumeration ──────────────────────────────────
static std::vector<CoreCoord> compute_grid_workers(distributed::MeshDevice* mesh) {
    CoreCoord g = mesh->compute_with_storage_grid_size();
    std::vector<CoreCoord> out; out.reserve(g.x * g.y);
    for (uint32_t y = 0; y < g.y; y++)
        for (uint32_t x = 0; x < g.x; x++) out.push_back({x, y});
    return out;
}

// ─── per-bank target ──────────────────────────────────────────────────
struct BankTarget {
    uint32_t channel_idx;
    uint32_t victim_row;
    uint32_t victim_addr;
    std::vector<uint32_t> aggr_addrs;
};
static BankTarget make_target(uint32_t ch_idx, uint32_t start_row,
                              uint32_t num_sides, uint32_t min_safe_row) {
    if (start_row < min_safe_row) start_row = min_safe_row;
    uint32_t bank_start = (start_row / ROWS_PER_BANK) * ROWS_PER_BANK;
    uint32_t v_row = bank_start + ROWS_PER_BANK / 2;
    BankTarget t{ ch_idx, v_row, v_row * ROW_SIZE, {} };
    for (uint32_t d = 1; t.aggr_addrs.size() < num_sides && d < ROWS_PER_BANK; d++) {
        if (v_row >= d + bank_start)
            t.aggr_addrs.push_back((v_row - d) * ROW_SIZE);
        if (t.aggr_addrs.size() < num_sides && v_row + d <= bank_start + ROWS_PER_BANK - 1)
            t.aggr_addrs.push_back((v_row + d) * ROW_SIZE);
    }
    return t;
}

// ─── DRAM row inspection via inspector kernel ─────────────────────────
static std::vector<uint32_t> inspect_victim_row(
    distributed::MeshDevice* mesh, IDevice* dev,
    const SubPort& sp, uint32_t victim_addr,
    CoreCoord inspector_core, uint32_t l1_dst_addr)
{
    auto& cq = mesh->mesh_command_queue();
    Program p = CreateProgram();
    KernelHandle k = CreateKernel(
        p, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/victim_inspect_kernel.cpp",
        inspector_core,
        DataMovementConfig{
            .processor = DataMovementProcessor::RISCV_0,
            .noc       = NOC::RISCV_0_default});
    SetRuntimeArgs(p, k, inspector_core, {sp.noc_x, sp.noc_y, victim_addr,
                                          CACHELINES_PER_ROW, l1_dst_addr});
    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(p));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);

    std::vector<uint32_t> out;
    detail::ReadFromDeviceL1(dev, inspector_core, l1_dst_addr, ROW_SIZE, out);
    return out;
}

static uint64_t fnv1a64(const std::vector<uint32_t>& v) {
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
    size_t n = v.size() * sizeof(uint32_t);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static void dump_first_last(const std::vector<uint32_t>& row, const char* label) {
    fmt::print("    {} hash: 0x{:016x}\n", label, fnv1a64(row));
    fmt::print("    {} CL[0]    (words 0-3):  {:08x} {:08x} {:08x} {:08x}\n",
               label, row[0], row[1], row[2], row[3]);
    uint32_t last_cl_word0 = (CACHELINES_PER_ROW - 1) * (CACHELINE / sizeof(uint32_t));
    fmt::print("    {} CL[127]  (words 0-3):  {:08x} {:08x} {:08x} {:08x}\n",
               label,
               row[last_cl_word0+0], row[last_cl_word0+1],
               row[last_cl_word0+2], row[last_cl_word0+3]);
}

int main(int argc, char** argv) {
    uint32_t tiles_per_sp   = 16;
    uint32_t num_banks      = 8;
    uint32_t num_subports   = 1;          // 1 = legacy single-endpoint behavior
    uint32_t start_row      = 2000;
    uint32_t num_sides      = 8;
    uint32_t hammer_iters   = 5000000;
    uint32_t data_pattern   = 0x55555555;

    for (int i = 1; i < argc; i++) {
        if ((!std::strcmp(argv[i], "--tiles-per-sp") || !std::strcmp(argv[i], "--cores-per-bank"))
            && i+1 < argc) tiles_per_sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--banks") && i+1 < argc)     num_banks = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sub-ports") && i+1 < argc) num_subports = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--start-row") && i+1 < argc) start_row = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--num-sides") && i+1 < argc) num_sides = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations") && i+1 < argc) hammer_iters = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pattern") && i+1 < argc) data_pattern = std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--banks N] [--sub-ports K] [--tiles-per-sp N]\n"
                       "              [--start-row N] [--num-sides N] [--iterations N] [--pattern HEX]\n"
                       "  Total tiles = banks * sub-ports * tiles-per-sp\n"
                       "  --sub-ports 1 = legacy middle endpoint only (matches prior runs)\n"
                       "  --sub-ports 3 = engage all 3 NOC endpoints per channel\n", argv[0]);
            return 0;
        }
    }
    if (num_banks > NUM_CHANNELS) num_banks = NUM_CHANNELS;
    if (num_subports < 1) num_subports = 1;
    if (num_subports > 3) num_subports = 3;

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq  = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];

    auto workers = compute_grid_workers(mesh.get());
    uint32_t need_tiles = num_banks * num_subports * tiles_per_sp;
    if (workers.size() < need_tiles) {
        fmt::print(stderr, "ERROR: need {} tiles but only {} available\n",
                   need_tiles, workers.size());
        return 1;
    }
    const auto& sp_order = SP_ORDER[num_subports];

    // L1 layout: BRISC scratch + result, then NCRISC scratch + result, then
    // an inspection buffer (8 KB) on the inspector core.
    uint32_t l1_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t result_size  = RESULT_BUF_WORDS * sizeof(uint32_t);
    uint32_t b_scratch = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t b_result  = (b_scratch + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_scratch = (b_result + result_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t n_result  = (n_scratch + 3 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t inspect_buf = (n_result + result_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;

    // Pre-compute per-bank targets (one per channel)
    std::vector<BankTarget> targets;
    for (uint32_t b = 0; b < num_banks; b++)
        targets.push_back(make_target(b, start_row, num_sides, min_safe_row));

    uint32_t threads_per_bank = num_subports * tiles_per_sp * 2;
    uint32_t total_threads = num_banks * threads_per_bank;
    uint64_t total_acts_target = static_cast<uint64_t>(total_threads) * hammer_iters * num_sides;

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Peak-Config Rowhammer Attack (dual-RISC, sub-ports)   ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Banks:            {} (channels)\n", num_banks);
    fmt::print("║  Sub-ports/bank:   {} (NOC endpoints per channel)\n", num_subports);
    fmt::print("║  Tiles/sub-port:   {} (× 2 RISCs)\n", tiles_per_sp);
    fmt::print("║  Threads/bank:     {}\n", threads_per_bank);
    fmt::print("║  Total tiles:      {}\n", num_banks * num_subports * tiles_per_sp);
    fmt::print("║  Total threads:    {}\n", total_threads);
    fmt::print("║  Aggressors/bank:  {}\n", num_sides);
    fmt::print("║  Iterations:       {} per thread\n", hammer_iters);
    fmt::print("║  Pattern:          0x{:08x}\n", data_pattern);
    fmt::print("║  Total acts:       {} (≈ {:.2f}B)\n", total_acts_target, total_acts_target / 1e9);
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    // ─── Step 1: pre-attack victim inspection (via sub-port 0 of each bank) ──
    CoreCoord inspector = workers[0];
    fmt::print("[Step 1] Pre-attack victim inspection (raw, pre-pattern-write)\n");
    for (uint32_t b = 0; b < num_banks; b++) {
        const auto& tgt = targets[b];
        const auto& ch  = CHANNELS[tgt.channel_idx];
        const auto& sp  = ch.sp[sp_order[0]];
        fmt::print("  bank{} {} row {} (0x{:08x}):\n", b, ch.name, tgt.victim_row, tgt.victim_addr);
        auto pre = inspect_victim_row(mesh.get(), dev, sp, tgt.victim_addr, inspector, inspect_buf);
        dump_first_last(pre, "      pre-write");
    }

    // ─── Step 2: pre-attack ECC ──
    fmt::print("\n[Step 2] Pre-attack ECC counters\n");
    EccCounters ecc_pre = read_ecc();
    if (ecc_pre.valid)
        fmt::print("  corr ch01:{} ch23:{} ch45:{} ch67:{}  uncorr:{}\n",
                   ecc_pre.corr[0], ecc_pre.corr[1], ecc_pre.corr[2], ecc_pre.corr[3], ecc_pre.uncorr);
    else
        fmt::print("  WARNING: pyluwen unavailable, ECC counters not read\n");

    // ─── Step 3: launch hammer (kernel writes pattern, hammers, verifies) ──
    fmt::print("\n[Step 3] Launching hammer ({} threads, {} acts/thread)…\n",
               total_threads, hammer_iters * num_sides);

    uint32_t tiles_per_bank = num_subports * tiles_per_sp;
    uint32_t total_tiles = num_banks * tiles_per_bank;
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

    // Tile assignment: bank b → sub-port s → tile i within that sub-port.
    // Global tile index = b * tiles_per_bank + s * tiles_per_sp + i.
    // Per-bank primary thread is BRISC of (s=0, i=0) tile in each bank;
    // it writes the pattern and verifies the victim.
    for (uint32_t b = 0; b < num_banks; b++) {
        const auto& tgt = targets[b];
        const auto& ch  = CHANNELS[tgt.channel_idx];
        for (uint32_t s = 0; s < num_subports; s++) {
            const auto& sp = ch.sp[sp_order[s]];
            for (uint32_t i = 0; i < tiles_per_sp; i++) {
                uint32_t global_tile = b * tiles_per_bank + s * tiles_per_sp + i;
                bool is_primary = (s == 0 && i == 0);
                uint32_t b_core_id = is_primary ? 0 : (global_tile * 2 + 100);
                uint32_t n_core_id = global_tile * 2 + 101;
                std::vector<uint32_t> b_args = {
                    sp.noc_x, sp.noc_y, tgt.victim_addr,
                    hammer_iters, data_pattern,
                    b_scratch, b_result,
                    b_core_id,
                    static_cast<uint32_t>(tgt.aggr_addrs.size()),
                };
                for (auto a : tgt.aggr_addrs) b_args.push_back(a);
                SetRuntimeArgs(prog, b_kid, sel[global_tile], b_args);

                std::vector<uint32_t> n_args = {
                    sp.noc_x, sp.noc_y, tgt.victim_addr,
                    hammer_iters, data_pattern,
                    n_scratch, n_result,
                    n_core_id,
                    static_cast<uint32_t>(tgt.aggr_addrs.size()),
                };
                for (auto a : tgt.aggr_addrs) n_args.push_back(a);
                SetRuntimeArgs(prog, n_kid, sel[global_tile], n_args);
            }
        }
    }

    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);
    fmt::print("  hammer complete\n");

    // ─── Step 4: post-attack ECC ──
    fmt::print("\n[Step 4] Post-attack ECC counters\n");
    EccCounters ecc_post = read_ecc();
    if (ecc_post.valid)
        fmt::print("  corr ch01:{} ch23:{} ch45:{} ch67:{}  uncorr:{}\n",
                   ecc_post.corr[0], ecc_post.corr[1], ecc_post.corr[2], ecc_post.corr[3], ecc_post.uncorr);
    print_ecc_delta(ecc_pre, ecc_post);

    // ─── Step 5: per-bank verification + post-attack inspection ──
    fmt::print("\n[Step 5] Per-bank verification (kernel reports + host re-read)\n");
    uint64_t total_acts = 0, max_cycles = 0;
    uint32_t total_kernel_flips = 0;
    for (uint32_t b = 0; b < num_banks; b++) {
        const auto& tgt = targets[b];
        const auto& ch  = CHANNELS[tgt.channel_idx];
        const auto& primary_sp = ch.sp[sp_order[0]];

        // Primary thread = BRISC of (s=0, i=0) tile within this bank
        CoreCoord primary = sel[b * tiles_per_bank];
        std::vector<uint32_t> rv;
        detail::ReadFromDeviceL1(dev, primary, b_result, result_size, rv);
        uint32_t flips    = rv[1];
        uint32_t bitflips = rv[2];
        uint64_t cycles   = (static_cast<uint64_t>(rv[4]) << 32) | rv[3];
        if (cycles > max_cycles) max_cycles = cycles;

        // Aggregate per-thread acts across all tiles in this bank
        uint64_t bank_acts = 0;
        for (uint32_t t = 0; t < tiles_per_bank; t++) {
            std::vector<uint32_t> rb, rn;
            detail::ReadFromDeviceL1(dev, sel[b*tiles_per_bank+t], b_result, result_size, rb);
            detail::ReadFromDeviceL1(dev, sel[b*tiles_per_bank+t], n_result, result_size, rn);
            bank_acts += rb[5] + rn[5];
        }
        total_acts += bank_acts;
        total_kernel_flips += flips;

        fmt::print("  bank{} {} victim row {}: {} flipped CLs, {} bit-flips, "
                   "{} acts (kernel time {:.1f} ms)\n",
                   b, ch.name, tgt.victim_row, flips, bitflips, bank_acts,
                   cycles * NS_PER_CYCLE / 1e6);

        if (flips > 0) {
            uint32_t n = std::min(flips, MAX_FLIP_RECORDS);
            for (uint32_t r = 0; r < n; r++) {
                uint32_t base = RESULT_HDR_WORDS + r * FLIP_RECORD_WORDS;
                fmt::print("    flip CL{} word{}: expected 0x{:08x} got 0x{:08x}\n",
                           rv[base], rv[base+1], rv[base+2], rv[base+3]);
            }
        }

        // Host-side re-read for an independent confirmation
        auto post = inspect_victim_row(mesh.get(), dev, primary_sp, tgt.victim_addr, inspector, inspect_buf);
        dump_first_last(post, "      post-attack");
        uint32_t host_flips = 0;
        for (uint32_t w = 0; w < post.size(); w++)
            if (post[w] != data_pattern) host_flips++;
        if (host_flips != flips)
            fmt::print("    NOTE: host saw {} mismatched words vs kernel's {} flipped CLs\n",
                       host_flips, flips);
    }

    fmt::print("\n[Summary]\n");
    fmt::print("  Confirmed activations:  {} (~{:.2f}B)\n", total_acts, total_acts / 1e9);
    fmt::print("  Aggregate rate:         {:.2f} M act/s\n",
               max_cycles > 0 ? total_acts / (max_cycles * NS_PER_CYCLE / 1e9) / 1e6 : 0.0);
    fmt::print("  Bit flips (kernel):     {}\n", total_kernel_flips);
    fmt::print("  ECC corrected delta:    {}\n",
               (ecc_pre.valid && ecc_post.valid) ? ecc_post.total_corr() - ecc_pre.total_corr() : 0u);
    fmt::print("  ECC uncorrectable:      {}\n",
               (ecc_pre.valid && ecc_post.valid) ? ecc_post.uncorr - ecc_pre.uncorr : 0u);

    mesh->close();
    return 0;
}
