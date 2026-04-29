// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// N-sided multi-core rowhammer host program for Tenstorrent Blackhole GDDR6.
//
// This program extends the basic rowhammer test with two key improvements:
//
// 1. N-SIDED HAMMERING (TRR evasion):
//    Instead of 2 aggressors (double-sided), uses N aggressors from the
//    same DRAM bank.  The aggressors are cycled in round-robin to overflow
//    the TRR (Target Row Refresh) counter table, which typically tracks
//    only 1-16 "hot" rows.  With N > TRR capacity, the real aggressors
//    (V-1 and V+1) may evade detection and cause unmitigated bit flips.
//
// 2. MULTI-CORE PARALLEL HAMMERING:
//    Launches the hammer kernel on M Tensix cores simultaneously, all
//    targeting the same DRAM channel.  This multiplies the NOC traffic
//    to the bank, increasing effective activation rate.  Core 0 handles
//    pattern writes and verification; other cores only hammer.
//
// === DRAM bank geometry ===
//    From TT-Rowhammer characterisation:
//    - Row size: 8 KB
//    - Same-bank rows: addresses differing in bits 13-16 (873 cycle latency)
//    - Cross-bank: bit 17+ change (897 cycles)
//    - Bank size: 16 rows (128 KB)
//    - Max same-bank aggressors: 15 (all rows in bank except victim)
//
// 3. REF-SYNC TIMING (TRR timing evasion):
//    When --delay or --delay-sweep is given, uses a combined N-sided +
//    REF-sync kernel that inserts a calibrated delay between aggressor
//    sweeps, shifting activations relative to DRAM auto-refresh windows.
//
// 4. EMPIRICAL ROW SETS (DRAMA-style):
//    When --row-set-file is given, aggressors come from the measured
//    same-bank set (produced by build_row_set) instead of the assumed
//    ROWS_PER_BANK=16 model.
//
// 5. STARTUP GEOMETRY VALIDATION:
//    On launch, probes ROWS_PER_BANK using the geometry_probe_kernel.
//    Hard-warns if measured value != 16 and uses the measured value.
//    Skip with --skip-geometry.
//
// Usage:
//   ./metal_example_rowhammer_nsided [options]
//
//   --channel N        DRAM channel 0-7 (default: 0)
//   --start-row N      First victim row (default: 1000)
//   --num-rows N       Rows to sweep (default: 16)
//   --iterations N     Sweeps through aggressors per row (default: 5000000)
//   --pattern 0xNN     Victim data pattern (default: 0x55555555)
//   --num-sides N      Aggressors, 2-30 (default: 14; >15 adds cross-bank dummies)
//   --num-cores N      Tensix cores, 1-8 (default: 4)
//   --barrier          Use serialized reads
//   --all-channels     Sweep all 8 channels
//   --delay N          REF-sync delay per sweep (BRISC cycles, 0=off)
//   --delay-sweep      Sweep delay_min..delay_max by delay_step (calibration)
//   --delay-min/max/step  Tune the delay sweep range
//   --row-set-file F   Use empirical aggressor addrs from build_row_set
//   --skip-geometry    Skip startup ROWS_PER_BANK validation

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fmt/core.h>
#include <string>
#include <vector>

#include <tt-metalium/host_api.hpp>
#include <tt-metalium/tt_metal.hpp>
#include <tt-metalium/device.hpp>
#include <tt-metalium/distributed.hpp>
#include <tt-metalium/allocator.hpp>
#include <tt-metalium/hal_types.hpp>

using namespace tt;
using namespace tt::tt_metal;

// ─── ECC counter monitoring via pyluwen ───────────────────────────────
struct EccCounters {
    uint32_t corr[4];
    uint32_t uncorr;
    bool valid;
    uint32_t total_corr() const { return corr[0] + corr[1] + corr[2] + corr[3]; }
};

static EccCounters read_ecc_counters() {
    EccCounters ec{};
    ec.valid = false;

    const char* cmd =
        "python3 -c \""
        "from pyluwen import PciChip; "
        "t=PciChip(pci_interface=0).get_telemetry(); "
        "print(t.gddr01_corr_errs, t.gddr23_corr_errs, "
              "t.gddr45_corr_errs, t.gddr67_corr_errs, "
              "t.gddr_uncorr_errs)"
        "\" 2>/dev/null";

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return ec;

    char buf[256];
    if (fgets(buf, sizeof(buf), pipe)) {
        int parsed = sscanf(buf, "%u %u %u %u %u",
                            &ec.corr[0], &ec.corr[1], &ec.corr[2], &ec.corr[3], &ec.uncorr);
        if (parsed == 5) ec.valid = true;
    }
    pclose(pipe);
    return ec;
}

static void print_ecc_delta(const char* label, const EccCounters& before, const EccCounters& after) {
    if (!before.valid || !after.valid) {
        fmt::print("  ECC {}: counters unavailable\n", label);
        return;
    }
    uint32_t d01 = after.corr[0] - before.corr[0];
    uint32_t d23 = after.corr[1] - before.corr[1];
    uint32_t d45 = after.corr[2] - before.corr[2];
    uint32_t d67 = after.corr[3] - before.corr[3];
    uint32_t du  = after.uncorr  - before.uncorr;
    uint32_t total_corr = d01 + d23 + d45 + d67;

    if (total_corr > 0 || du > 0) {
        fmt::print("  *** ECC {}: +{} corrected (ch01:{} ch23:{} ch45:{} ch67:{}), +{} uncorrectable\n",
                   label, total_corr, d01, d23, d45, d67, du);
        if (total_corr > 0) {
            fmt::print("    -> ECC is SILENTLY CORRECTING bit flips!\n");
        }
        if (du > 0) {
            fmt::print("    -> UNCORRECTABLE multi-bit errors detected!\n");
        }
    } else {
        fmt::print("  ECC {}: no new errors\n", label);
    }
}

// ─── constants ────────────────────────────────────────────────────────
static constexpr uint32_t ROW_SIZE         = 8192;
static constexpr uint32_t CACHELINE        = 64;
static constexpr double   NS_PER_CYCLE     = 1.25;
static constexpr uint32_t ROWS_PER_BANK    = 16;    // from address bit mapping
static constexpr uint32_t MAX_AGGRESSORS   = 30;    // kernel limit

static constexpr uint32_t RESULT_HDR_WORDS   = 6;
static constexpr uint32_t MAX_FLIP_RECORDS   = 32;
static constexpr uint32_t FLIP_RECORD_WORDS  = 4;
static constexpr uint32_t RESULT_BUF_WORDS   = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

// Geometry probe constants (for ROWS_PER_BANK validation)
static constexpr uint32_t GEOPROBE_MAX_PROBES  = 64;
static constexpr uint32_t GEOPROBE_RESULT_WORDS = 1 + GEOPROBE_MAX_PROBES;
static constexpr uint32_t GEOPROBE_SCRATCH_BYTES = 2 * 1024;  // matches geometry_probe_kernel
static constexpr uint32_t REF_DIFF_BANKGRP_CYC = 897;
static constexpr uint32_t TIER_TOLERANCE        = 25;

// ─── DRAM channel NOC coordinates ─────────────────────────────────────
struct DramChannel {
    uint32_t noc_x;
    uint32_t noc_y;
    const char* name;
};

static const DramChannel DRAM_CHANNELS[] = {
    {0,  1,  "ch0 (0,1)" },
    {0,  10, "ch1 (0,10)"},
    {0,  4,  "ch2 (0,4)" },
    {0,  7,  "ch3 (0,7)" },
    {9,  1,  "ch4 (9,1)" },
    {9,  10, "ch5 (9,10)"},
    {9,  4,  "ch6 (9,4)" },
    {9,  7,  "ch7 (9,7)" },
};

// ─── aggressor selection ──────────────────────────────────────────────
// Select N aggressors, prioritizing the same DRAM bank as the victim.
// Bank = victim_row / ROWS_PER_BANK (bits 16:13 of address).
// Priority: adjacent rows (V-1, V+1) first, then outward within bank,
// then cross-bank dummy rows to overflow TRR counters.
static std::vector<uint32_t> select_same_bank_aggressors(
    uint32_t victim_row, uint32_t num_sides, uint32_t min_safe_row,
    uint32_t measured_rows_per_bank) {

    std::vector<uint32_t> aggrs;
    uint32_t rpb = measured_rows_per_bank;
    uint32_t bank_start = (victim_row / rpb) * rpb;
    uint32_t bank_end   = bank_start + rpb - 1;

    // Phase 1: same-bank aggressors (closest first) — these cause actual flips
    for (uint32_t dist = 1; dist < rpb && aggrs.size() < num_sides; dist++) {
        if (victim_row >= dist + bank_start) {
            uint32_t r = victim_row - dist;
            if (r >= bank_start && r >= min_safe_row) {
                aggrs.push_back(r);
            }
        }
        if (aggrs.size() < num_sides) {
            uint32_t r = victim_row + dist;
            if (r <= bank_end) {
                aggrs.push_back(r);
            }
        }
    }

    // Phase 2: cross-bank dummy aggressors (for TRR overflow).
    // These don't cause flips on the victim but they pollute the TRR
    // sampler's counter table, potentially evicting the real V±1 entries.
    // Pull from adjacent banks above and below.
    for (uint32_t dist = 1; aggrs.size() < num_sides; dist++) {
        // Bank above
        uint32_t above = bank_end + dist;
        if (above >= min_safe_row && aggrs.size() < num_sides) {
            aggrs.push_back(above);
        }
        // Bank below
        if (bank_start >= dist + min_safe_row && aggrs.size() < num_sides) {
            uint32_t below = bank_start - dist;
            if (below >= min_safe_row) {
                aggrs.push_back(below);
            }
        }
        if (dist > 256) break;  // safety limit
    }

    if (aggrs.size() > MAX_AGGRESSORS) aggrs.resize(MAX_AGGRESSORS);
    return aggrs;
}

// Load empirical row set from file (one byte-offset per line, # comments).
static std::vector<uint32_t> load_row_set_file(const std::string& path) {
    std::vector<uint32_t> addrs;
    std::ifstream f(path);
    if (!f) {
        fmt::print(stderr, "Error: cannot open row-set file: {}\n", path);
        return addrs;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        uint32_t val = static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 0));
        if (val > 0) addrs.push_back(val);
    }
    return addrs;
}

// ─── ROWS_PER_BANK runtime validation ────────────────────────────────
// Probes anchor vs anchor + k*8KB for k=1..32 using geometry_probe_kernel.
// Returns measured ROWS_PER_BANK (first k where latency jumps to cross-bank tier).
static uint32_t measure_rows_per_bank(
    distributed::MeshDevice& mesh_device,
    distributed::MeshCommandQueue& cq,
    IDevice* device,
    CoreCoord worker_core,
    uint32_t geo_scratch_addr,
    uint32_t geo_result_addr,
    const DramChannel& ch,
    uint32_t safe_addr)
{
    // Probe anchor vs anchor + k*8KB for k=1..32
    constexpr uint32_t K_MAX = 32;
    Program program = CreateProgram();
    KernelHandle kid = CreateKernel(
        program,
        OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/geometry_probe_kernel.cpp",
        worker_core,
        DataMovementConfig{
            .processor = DataMovementProcessor::RISCV_0,
            .noc       = NOC::RISCV_0_default});

    std::vector<uint32_t> args;
    args.reserve(5 + 2 * K_MAX);
    args.push_back(ch.noc_x);
    args.push_back(ch.noc_y);
    args.push_back(geo_scratch_addr);
    args.push_back(geo_result_addr);
    args.push_back(K_MAX);
    for (uint32_t k = 1; k <= K_MAX; ++k) {
        args.push_back(safe_addr);
        args.push_back(safe_addr + k * ROW_SIZE);
    }
    SetRuntimeArgs(program, kid, worker_core, args);

    distributed::MeshWorkload workload;
    workload.add_program(distributed::MeshCoordinateRange(mesh_device.shape()),
                         std::move(program));
    distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
    distributed::Finish(cq);

    std::vector<uint32_t> result_vec;
    detail::ReadFromDeviceL1(device, worker_core, geo_result_addr,
                             GEOPROBE_RESULT_WORDS * sizeof(uint32_t), result_vec);

    // Find first k where latency jumps to cross-bank-group tier
    uint32_t measured = 0;
    fmt::print("  B2 probe (anchor vs anchor+k·8KB):\n");
    for (uint32_t k = 1; k <= K_MAX; ++k) {
        uint32_t cyc = result_vec[k];
        const char* tag = (cyc >= REF_DIFF_BANKGRP_CYC - TIER_TOLERANCE) ? " <-- JUMP" : "";
        fmt::print("    k={:2d}: {:4d} cyc{}\n", k, cyc, tag);
        if (measured == 0 && cyc >= REF_DIFF_BANKGRP_CYC - TIER_TOLERANCE) {
            measured = k;
        }
    }
    return measured;
}

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

int main(int argc, char* argv[]) {
    // ─── configuration with defaults ──────────────────────────────────
    uint32_t channel_idx       = 0;
    uint32_t start_row         = 1000;
    uint32_t num_rows_to_test  = 16;
    uint32_t hammer_iterations = 5000000;
    uint32_t data_pattern      = 0x55555555;
    uint32_t num_sides         = 14;         // aggressors
    uint32_t num_cores         = 4;          // Tensix cores
    bool     all_channels      = false;
    uint32_t use_barrier       = 0;
    uint32_t delay_iters       = 0;          // REF-sync delay (0 = disabled)
    bool     delay_sweep       = false;      // sweep delay 0..256 step 8
    uint32_t delay_min         = 0;
    uint32_t delay_max         = 256;
    uint32_t delay_step        = 8;
    std::string row_set_file;                // empirical row set (C3)
    bool     skip_geometry     = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            channel_idx = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--start-row") == 0 && i + 1 < argc) {
            start_row = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--num-rows") == 0 && i + 1 < argc) {
            num_rows_to_test = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            hammer_iterations = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
            data_pattern = std::strtoul(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--num-sides") == 0 && i + 1 < argc) {
            num_sides = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--num-cores") == 0 && i + 1 < argc) {
            num_cores = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--all-channels") == 0) {
            all_channels = true;
        } else if (std::strcmp(argv[i], "--barrier") == 0) {
            use_barrier = 1;
        } else if (std::strcmp(argv[i], "--delay") == 0 && i + 1 < argc) {
            delay_iters = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--delay-sweep") == 0) {
            delay_sweep = true;
        } else if (std::strcmp(argv[i], "--delay-min") == 0 && i + 1 < argc) {
            delay_min = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--delay-max") == 0 && i + 1 < argc) {
            delay_max = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--delay-step") == 0 && i + 1 < argc) {
            delay_step = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--row-set-file") == 0 && i + 1 < argc) {
            row_set_file = argv[++i];
        } else if (std::strcmp(argv[i], "--skip-geometry") == 0) {
            skip_geometry = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [options]\n", argv[0]);
            fmt::print("  --channel N       DRAM channel 0-7 (default: 0)\n");
            fmt::print("  --start-row N     First victim row (default: 1000)\n");
            fmt::print("  --num-rows N      Rows to sweep (default: 16)\n");
            fmt::print("  --iterations N    Sweeps per aggressor set (default: 5000000)\n");
            fmt::print("  --pattern 0xNN    Victim data pattern (default: 0x55555555)\n");
            fmt::print("  --num-sides N     Aggressors, 2-30 (default: 14)\n");
            fmt::print("  --num-cores N     Tensix cores, 1-8 (default: 4)\n");
            fmt::print("  --all-channels    Sweep all 8 DRAM channels\n");
            fmt::print("  --barrier         Use serialized reads\n");
            fmt::print("  --delay N         REF-sync delay (BRISC cycles per sweep, 0=off)\n");
            fmt::print("  --delay-sweep     Sweep delay {} to {} step {} (calibration)\n",
                       delay_min, delay_max, delay_step);
            fmt::print("  --delay-min/max/step  Tune delay-sweep range\n");
            fmt::print("  --row-set-file F  Use empirical aggressor addrs from build_row_set\n");
            fmt::print("  --skip-geometry   Skip startup ROWS_PER_BANK validation\n");
            return 0;
        }
    }

    // ─── validation ───────────────────────────────────────────────────
    if (channel_idx >= 8) {
        fmt::print(stderr, "Error: channel must be 0-7\n");
        return 1;
    }
    if (num_sides < 2) num_sides = 2;
    if (num_sides > MAX_AGGRESSORS) {
        fmt::print("Warning: capping --num-sides to {} (kernel MAX_AGGRESSORS)\n", MAX_AGGRESSORS);
        num_sides = MAX_AGGRESSORS;
    }
    if (num_cores < 1) num_cores = 1;
    if (num_cores > 8) {
        fmt::print("Warning: capping --num-cores to 8\n");
        num_cores = 8;
    }
    bool use_refsync = (delay_iters > 0 || delay_sweep);

    const DramChannel& ch = DRAM_CHANNELS[channel_idx];
    uint32_t ch_start = all_channels ? 0 : channel_idx;
    uint32_t ch_end   = all_channels ? 8 : channel_idx + 1;

    // ─── banner ───────────────────────────────────────────────────────
    fmt::print("╔══════════════════════════════════════════════════════════╗\n");
    fmt::print("║     N-Sided Multi-Core Rowhammer (TRR Evasion)         ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════╣\n");
    if (all_channels) {
        fmt::print("║  Channels:     ALL (0-7)\n");
    } else {
        fmt::print("║  Channel:      {} ({})\n", channel_idx, ch.name);
        fmt::print("║  DRAM NOC:     ({}, {})\n", ch.noc_x, ch.noc_y);
    }
    fmt::print("║  Victim rows:  {} to {}\n", start_row, start_row + num_rows_to_test - 1);
    fmt::print("║  Iterations:   {} (sweeps through aggressor set)\n", hammer_iterations);
    fmt::print("║  Aggressors:   {} (N-sided{})\n", num_sides,
               num_sides > ROWS_PER_BANK - 1 ? ", with cross-bank dummies" : ", same-bank TRR evasion");
    fmt::print("║  Cores:        {} (parallel hammer)\n", num_cores);
    fmt::print("║  Pattern:      0x{:08x}  (aggr: 0x{:08x})\n", data_pattern, ~data_pattern);
    fmt::print("║  Mode:         {}\n", use_barrier ? "serialized (barrier)" : "pipelined (fast)");
    if (use_refsync) {
        if (delay_sweep) {
            fmt::print("║  REF sync:     sweep delay {}..{} step {}\n", delay_min, delay_max, delay_step);
        } else {
            fmt::print("║  REF sync:     delay={} BRISC cycles/sweep\n", delay_iters);
        }
    }
    if (!row_set_file.empty()) {
        fmt::print("║  Row set:      {} (empirical)\n", row_set_file);
    }
    fmt::print("║  Row size:     {} bytes (8 KB)\n", ROW_SIZE);
    fmt::print("║  Bank size:    {} rows (128 KB)\n", ROWS_PER_BANK);
    fmt::print("╚══════════════════════════════════════════════════════════╝\n\n");

    // ─── ECC before ───────────────────────────────────────────────────
    fmt::print("Reading ECC counters (before)...\n");
    EccCounters ecc_before = read_ecc_counters();
    if (ecc_before.valid) {
        fmt::print("  corr: ch01={} ch23={} ch45={} ch67={}  uncorr={}\n",
                   ecc_before.corr[0], ecc_before.corr[1],
                   ecc_before.corr[2], ecc_before.corr[3], ecc_before.uncorr);
    } else {
        fmt::print("  WARNING: Could not read ECC counters.\n");
    }
    fmt::print("\n");

    // ─── device setup ─────────────────────────────────────────────────
    constexpr int device_id = 0;
    std::shared_ptr<distributed::MeshDevice> mesh_device =
        distributed::MeshDevice::create_unit_mesh(device_id);
    distributed::MeshCommandQueue& cq = mesh_device->mesh_command_queue();

    IDevice* device = mesh_device->get_devices()[0];

    // ─── select worker cores ──────────────────────────────────────────
    // Use cores (1,2), (2,2), ..., (num_cores,2) — first row of compute grid.
    std::vector<CoreCoord> worker_cores;
    for (uint32_t c = 0; c < num_cores; c++) {
        worker_cores.push_back({1 + c, 2});
    }
    // CoreRange for kernel mapping (inclusive end)
    CoreRange core_range({1, 2}, {num_cores, 2});

    fmt::print("Worker cores: ");
    for (auto& c : worker_cores) {
        fmt::print("({},{}) ", c.x, c.y);
    }
    fmt::print("\n");

    // ─── L1 layout (same offset on all cores, each has own L1) ────────
    constexpr uint32_t scratch_size = 2 * CACHELINE;  // 128 bytes
    constexpr uint32_t result_size  = RESULT_BUF_WORDS * sizeof(uint32_t);

    uint32_t l1_base = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    constexpr uint32_t L1_ALIGN = 64;
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + scratch_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    fmt::print("L1 layout: base=0x{:x}  scratch=0x{:x}  result=0x{:x}\n",
               l1_base, scratch_addr, result_addr);

    // ─── DRAM safety check ────────────────────────────────────────────
    uint32_t dram_base = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    fmt::print("DRAM reserved: 0x0 - 0x{:x}\n", dram_base);
    fmt::print("Min safe victim row: {}\n", min_safe_row);

    if (start_row < min_safe_row) {
        fmt::print("WARNING: auto-adjusting start_row {} -> {}\n", start_row, min_safe_row);
        start_row = min_safe_row;
    }

    // ─── runtime ROWS_PER_BANK validation (B2) ───────────────────────
    uint32_t measured_rpb = ROWS_PER_BANK;  // fallback
    if (!skip_geometry) {
        fmt::print("\n── Validating ROWS_PER_BANK ────────────────────────────\n");
        uint32_t safe_anchor = ((dram_base / ROW_SIZE) + 64) * ROW_SIZE;

        // Need separate scratch area for geometry probe (larger than hammer scratch)
        uint32_t geo_scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
        uint32_t geo_result_addr  = (geo_scratch_addr + GEOPROBE_SCRATCH_BYTES + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

        measured_rpb = measure_rows_per_bank(
            *mesh_device, cq, device, worker_cores[0],
            geo_scratch_addr, geo_result_addr,
            DRAM_CHANNELS[channel_idx], safe_anchor);

        if (measured_rpb == 0) {
            fmt::print("  WARNING: no cross-bank-group jump found in k=1..32\n");
            fmt::print("  Falling back to hardcoded ROWS_PER_BANK={}\n", ROWS_PER_BANK);
            measured_rpb = ROWS_PER_BANK;
        } else if (measured_rpb != ROWS_PER_BANK) {
            fmt::print("\n  *** ROWS_PER_BANK MISMATCH ***\n");
            fmt::print("  Measured: {}   Hardcoded: {}\n", measured_rpb, ROWS_PER_BANK);
            fmt::print("  The n-sided code has been hammering wrong rows!\n");
            fmt::print("  Using measured value {} for this run.\n", measured_rpb);
        } else {
            fmt::print("  ROWS_PER_BANK = {} confirmed (matches constant)\n", measured_rpb);
        }
        fmt::print("────────────────────────────────────────────────────────\n\n");
    }

    // Load empirical row set if provided
    std::vector<uint32_t> empirical_addrs;
    if (!row_set_file.empty()) {
        empirical_addrs = load_row_set_file(row_set_file);
        if (empirical_addrs.empty()) {
            fmt::print(stderr, "Error: row-set file is empty or unreadable\n");
            return 1;
        }
        fmt::print("Loaded {} empirical aggressor addresses from {}\n",
                   empirical_addrs.size(), row_set_file);
    }

    // Ensure victim is in the middle of its bank for maximum aggressor coverage
    uint32_t victim_bank_pos = start_row % measured_rpb;
    if (victim_bank_pos < 2 || victim_bank_pos > measured_rpb - 3) {
        uint32_t bank_start_row = (start_row / measured_rpb) * measured_rpb;
        uint32_t ideal_start = bank_start_row + measured_rpb / 2;
        if (ideal_start < min_safe_row) {
            ideal_start = ((min_safe_row / measured_rpb) + 1) * measured_rpb + measured_rpb / 2;
        }
        fmt::print("Note: shifting start_row {} -> {} (center of bank for max aggressor coverage)\n",
                   start_row, ideal_start);
        start_row = ideal_start;
    }
    fmt::print("\n");

    // ─── tracking ─────────────────────────────────────────────────────
    uint32_t total_flips_found = 0;
    uint32_t rows_tested = 0;
    EccCounters ecc_batch_start = ecc_before;

    // ─── sweep ────────────────────────────────────────────────────────
    for (uint32_t ch_idx = ch_start; ch_idx < ch_end; ch_idx++) {
        const DramChannel& cur_ch = DRAM_CHANNELS[ch_idx];
        if (all_channels) {
            fmt::print("\n── Channel {} ({}) ──────────────────────────────────\n",
                       ch_idx, cur_ch.name);
        }

        for (uint32_t row = start_row; row < start_row + num_rows_to_test; row++) {
            uint32_t victim_addr = row * ROW_SIZE;

            // Select aggressors: empirical row set, or computed
            std::vector<uint32_t> aggr_addrs;
            uint32_t actual_sides;

            if (!empirical_addrs.empty()) {
                // C3: use empirical same-bank addresses from build_row_set
                aggr_addrs = empirical_addrs;
                if (aggr_addrs.size() > MAX_AGGRESSORS)
                    aggr_addrs.resize(MAX_AGGRESSORS);
                actual_sides = aggr_addrs.size();
            } else {
                auto aggr_rows = select_same_bank_aggressors(
                    row, num_sides, min_safe_row, measured_rpb);
                actual_sides = aggr_rows.size();

                if (actual_sides < 2) {
                    fmt::print("   Row {:5d}: skipped (insufficient aggressors)\n", row);
                    continue;
                }

                for (uint32_t r : aggr_rows) {
                    aggr_addrs.push_back(r * ROW_SIZE);
                }

                // Print aggressor info on first row
                if (row == start_row) {
                    uint32_t same_bank_count = 0;
                    uint32_t bank_s = (row / measured_rpb) * measured_rpb;
                    uint32_t bank_e = bank_s + measured_rpb - 1;
                    for (uint32_t r : aggr_rows) {
                        if (r >= bank_s && r <= bank_e) same_bank_count++;
                    }
                    fmt::print("  Aggressors for row {}: {} total ({} same-bank, {} cross-bank dummies)\n",
                               row, actual_sides, same_bank_count, actual_sides - same_bank_count);
                    fmt::print("  Bank range: {}-{}  (measured RPB={})\n",
                               bank_s, bank_e, measured_rpb);
                }
            }

            // Build list of delay values to sweep
            std::vector<uint32_t> delays;
            if (delay_sweep) {
                for (uint32_t d = delay_min; d <= delay_max; d += delay_step) {
                    delays.push_back(d);
                }
            } else {
                delays.push_back(delay_iters);
            }

            for (uint32_t cur_delay : delays) {

            // ── create program ────────────────────────────────────────
            Program program = CreateProgram();

            // Choose kernel: refsync variant if delay > 0
            const char* kernel_path = (cur_delay > 0)
                ? OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_nsided_refsync_kernel.cpp"
                : OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_nsided_kernel.cpp";

            KernelHandle kernel_id = CreateKernel(
                program,
                kernel_path,
                core_range,
                DataMovementConfig{
                    .processor = DataMovementProcessor::RISCV_0,
                    .noc = NOC::RISCV_0_default});

            // Set per-core runtime args
            for (uint32_t c = 0; c < num_cores; c++) {
                std::vector<uint32_t> args = {
                    cur_ch.noc_x,       // arg 0
                    cur_ch.noc_y,       // arg 1
                    victim_addr,        // arg 2
                    hammer_iterations,  // arg 3
                    data_pattern,       // arg 4
                    scratch_addr,       // arg 5
                    result_addr,        // arg 6
                    use_barrier,        // arg 7
                    actual_sides,       // arg 8: num_aggressors
                    c,                  // arg 9: core_id (0 = primary)
                    cur_delay,          // arg 10: delay_iters (0 for plain kernel)
                };
                // Append aggressor addresses (args 11+)
                for (uint32_t addr : aggr_addrs) {
                    args.push_back(addr);
                }

                SetRuntimeArgs(program, kernel_id, worker_cores[c], args);
            }

            // Execute on all cores simultaneously
            distributed::MeshWorkload workload;
            distributed::MeshCoordinateRange device_range =
                distributed::MeshCoordinateRange(mesh_device->shape());
            workload.add_program(device_range, std::move(program));
            distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
            distributed::Finish(cq);

            // ── read results from core 0 (has flip data) ──────────────
            std::vector<uint32_t> result_vec;
            detail::ReadFromDeviceL1(device, worker_cores[0], result_addr, result_size, result_vec);

            uint32_t num_flips       = result_vec[1];
            uint32_t total_bit_flips = result_vec[2];
            uint32_t cycles_lo       = result_vec[3];
            uint32_t cycles_hi       = result_vec[4];
            uint64_t cycles          = (static_cast<uint64_t>(cycles_hi) << 32) | cycles_lo;

            // Aggregate activation counts from all cores
            uint64_t total_activations = 0;
            for (uint32_t c = 0; c < num_cores; c++) {
                std::vector<uint32_t> core_result;
                detail::ReadFromDeviceL1(device, worker_cores[c], result_addr, result_size, core_result);
                total_activations += core_result[5];
            }

            double act_rate = (total_activations > 0 && cycles > 0)
                                ? (static_cast<double>(total_activations) / (cycles * NS_PER_CYCLE / 1e9))
                                : 0.0;

            rows_tested++;

            std::string delay_tag = (cur_delay > 0)
                ? fmt::format(", delay={}", cur_delay)
                : "";

            if (num_flips > 0) {
                total_flips_found += num_flips;
                fmt::print("██ ROW {:5d} (0x{:06x}): {} BIT FLIPS! ({} words) "
                           "[{} acts, {} sides, {} cores, {:.2f} M act/s{}]\n",
                           row, victim_addr, total_bit_flips, num_flips,
                           total_activations, actual_sides, num_cores, act_rate / 1e6,
                           delay_tag);

                uint32_t records = std::min(num_flips, MAX_FLIP_RECORDS);
                for (uint32_t r = 0; r < records; r++) {
                    uint32_t base = RESULT_HDR_WORDS + r * FLIP_RECORD_WORDS;
                    uint32_t cl_idx   = result_vec[base + 0];
                    uint32_t word_off = result_vec[base + 1];
                    uint32_t expected = result_vec[base + 2];
                    uint32_t actual   = result_vec[base + 3];
                    uint32_t diff     = expected ^ actual;
                    uint32_t byte_off = cl_idx * CACHELINE + word_off * sizeof(uint32_t);
                    fmt::print("   Flip #{}: offset 0x{:04x} (CL {} word {}): "
                               "exp 0x{:08x}, got 0x{:08x}, diff 0x{:08x}\n",
                               r + 1, byte_off, cl_idx, word_off, expected, actual, diff);
                }
                fmt::print("\n");
            } else {
                fmt::print("   Row {:5d} (0x{:06x}): no flips  "
                           "[{} total acts, {} sides, {} cores, {:.2f} M act/s{}]\n",
                           row, victim_addr, total_activations, actual_sides, num_cores,
                           act_rate / 1e6, delay_tag);
            }

            } // end delay sweep

            // ── periodic ECC check (every 4 rows) ─────────────────────
            if (rows_tested % 4 == 0) {
                EccCounters ecc_now = read_ecc_counters();
                if (ecc_now.valid && ecc_batch_start.valid) {
                    uint32_t batch_corr =
                        (ecc_now.corr[0] - ecc_batch_start.corr[0]) +
                        (ecc_now.corr[1] - ecc_batch_start.corr[1]) +
                        (ecc_now.corr[2] - ecc_batch_start.corr[2]) +
                        (ecc_now.corr[3] - ecc_batch_start.corr[3]);
                    uint32_t batch_uncorr = ecc_now.uncorr - ecc_batch_start.uncorr;
                    if (batch_corr > 0 || batch_uncorr > 0) {
                        print_ecc_delta("batch", ecc_batch_start, ecc_now);
                    }
                    ecc_batch_start = ecc_now;
                }
            }
        }
    }

    // ─── final ECC ────────────────────────────────────────────────────
    fmt::print("\nReading ECC counters (after)...\n");
    EccCounters ecc_after = read_ecc_counters();
    if (ecc_after.valid) {
        fmt::print("  corr: ch01={} ch23={} ch45={} ch67={}  uncorr={}\n",
                   ecc_after.corr[0], ecc_after.corr[1],
                   ecc_after.corr[2], ecc_after.corr[3], ecc_after.uncorr);
    }

    // ─── summary ──────────────────────────────────────────────────────
    fmt::print("\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("  N-SIDED MULTI-CORE ROWHAMMER SUMMARY\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("  Rows tested:       {}\n", rows_tested);
    fmt::print("  Total bit flips:   {}\n", total_flips_found);
    fmt::print("  Aggressors/row:    {} (N-sided)\n", num_sides);
    fmt::print("  Cores:             {} (parallel)\n", num_cores);
    fmt::print("  Iterations:        {} per row\n", hammer_iterations);
    fmt::print("  Pattern:           0x{:08x}\n", data_pattern);
    fmt::print("  Mode:              {}\n", use_barrier ? "barrier" : "pipelined");
    fmt::print("───────────────────────────────────────────────────────────\n");

    if (ecc_before.valid && ecc_after.valid) {
        print_ecc_delta("total", ecc_before, ecc_after);
        uint32_t ecc_corr =
            (ecc_after.corr[0] - ecc_before.corr[0]) +
            (ecc_after.corr[1] - ecc_before.corr[1]) +
            (ecc_after.corr[2] - ecc_before.corr[2]) +
            (ecc_after.corr[3] - ecc_before.corr[3]);

        if (ecc_corr > 0 && total_flips_found == 0) {
            fmt::print("\n  *** ECC IS MASKING ROWHAMMER BIT FLIPS ***\n");
            fmt::print("  The N-sided attack IS causing errors, but ECC corrects them.\n");
            fmt::print("  Try more aggressors or higher iteration counts.\n");
        }
    }

    if (measured_rpb != ROWS_PER_BANK) {
        fmt::print("  Measured RPB:      {} (overrode hardcoded {})\n", measured_rpb, ROWS_PER_BANK);
    }
    if (!row_set_file.empty()) {
        fmt::print("  Row set:           {} ({} addrs)\n", row_set_file, empirical_addrs.size());
    }
    if (use_refsync) {
        if (delay_sweep) {
            fmt::print("  REF sync:          sweep {}..{} step {}\n", delay_min, delay_max, delay_step);
        } else {
            fmt::print("  REF sync delay:    {}\n", delay_iters);
        }
    }

    if (total_flips_found > 0) {
        fmt::print("\n  *** ROWHAMMER VULNERABILITY CONFIRMED ***\n");
    } else {
        fmt::print("\n  No bit flips detected. Consider:\n");
        fmt::print("    - Higher --iterations (try 50000000)\n");
        fmt::print("    - Different --start-row to hit other banks\n");
        fmt::print("    - --barrier mode for confirmed activations\n");
        fmt::print("    - Different --pattern (try 0x00000000 or 0xFFFFFFFF)\n");
        fmt::print("    - --delay-sweep to try REF synchronization (C1)\n");
        fmt::print("    - --row-set-file with empirical aggressors from build_row_set (C3)\n");
        fmt::print("    - --num-sides 18-24 to test GPUHammer-sized TRR\n");
    }
    fmt::print("═══════════════════════════════════════════════════════════\n");

    mesh_device->close();
    return (total_flips_found > 0) ? 0 : 1;
}
