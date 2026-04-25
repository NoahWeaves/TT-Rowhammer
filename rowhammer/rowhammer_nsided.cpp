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
// Usage:
//   ./metal_example_rowhammer_nsided [options]
//
//   --channel N       DRAM channel 0-7 (default: 0)
//   --start-row N     First victim row (default: 1000)
//   --num-rows N      Rows to sweep (default: 16)
//   --iterations N    Sweeps through aggressors per row (default: 5000000)
//   --pattern 0xNN    Victim data pattern (default: 0x55555555)
//   --num-sides N     Number of aggressors, 2-15 (default: 14)
//   --num-cores N     Tensix cores, 1-8 (default: 4)
//   --barrier         Use serialized reads
//   --all-channels    Sweep all 8 channels

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

static constexpr uint32_t RESULT_HDR_WORDS   = 6;
static constexpr uint32_t MAX_FLIP_RECORDS   = 32;
static constexpr uint32_t FLIP_RECORD_WORDS  = 4;
static constexpr uint32_t RESULT_BUF_WORDS   = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

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
// Select N aggressors from the same DRAM bank as the victim.
// Bank = victim_row / 16 (bits 16:13 of address).
// Priority: adjacent rows (V-1, V+1) first, then outward.
static std::vector<uint32_t> select_same_bank_aggressors(
    uint32_t victim_row, uint32_t num_sides, uint32_t min_safe_row) {

    std::vector<uint32_t> aggrs;
    uint32_t bank_start = (victim_row / ROWS_PER_BANK) * ROWS_PER_BANK;
    uint32_t bank_end   = bank_start + ROWS_PER_BANK - 1;

    // Add aggressors in order of distance from victim (closest first)
    for (uint32_t dist = 1; dist < ROWS_PER_BANK && aggrs.size() < num_sides; dist++) {
        // Below victim
        if (victim_row >= dist + bank_start) {
            uint32_t r = victim_row - dist;
            if (r >= bank_start && r >= min_safe_row) {
                aggrs.push_back(r);
            }
        }
        // Above victim
        if (aggrs.size() < num_sides) {
            uint32_t r = victim_row + dist;
            if (r <= bank_end) {
                aggrs.push_back(r);
            }
        }
    }

    return aggrs;
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
    uint32_t num_sides         = 14;         // aggressors (max same-bank = 15)
    uint32_t num_cores         = 4;          // Tensix cores
    bool     all_channels      = false;
    uint32_t use_barrier       = 0;

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
        } else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [options]\n", argv[0]);
            fmt::print("  --channel N       DRAM channel 0-7 (default: 0)\n");
            fmt::print("  --start-row N     First victim row (default: 1000)\n");
            fmt::print("  --num-rows N      Rows to sweep (default: 16)\n");
            fmt::print("  --iterations N    Sweeps per aggressor set (default: 5000000)\n");
            fmt::print("  --pattern 0xNN    Victim data pattern (default: 0x55555555)\n");
            fmt::print("  --num-sides N     Aggressors, 2-15 (default: 14)\n");
            fmt::print("  --num-cores N     Tensix cores, 1-8 (default: 4)\n");
            fmt::print("  --all-channels    Sweep all 8 DRAM channels\n");
            fmt::print("  --barrier         Use serialized reads\n");
            return 0;
        }
    }

    // ─── validation ───────────────────────────────────────────────────
    if (channel_idx >= 8) {
        fmt::print(stderr, "Error: channel must be 0-7\n");
        return 1;
    }
    if (num_sides < 2) num_sides = 2;
    if (num_sides > 15) {
        fmt::print("Warning: capping --num-sides to 15 (max same-bank rows)\n");
        num_sides = 15;
    }
    if (num_cores < 1) num_cores = 1;
    if (num_cores > 8) {
        fmt::print("Warning: capping --num-cores to 8\n");
        num_cores = 8;
    }

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
    fmt::print("║  Aggressors:   {} (N-sided, same-bank TRR evasion)\n", num_sides);
    fmt::print("║  Cores:        {} (parallel hammer)\n", num_cores);
    fmt::print("║  Pattern:      0x{:08x}  (aggr: 0x{:08x})\n", data_pattern, ~data_pattern);
    fmt::print("║  Mode:         {}\n", use_barrier ? "serialized (barrier)" : "pipelined (fast)");
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

    // Ensure victim is in the middle of its bank for maximum aggressor coverage
    uint32_t victim_bank_pos = start_row % ROWS_PER_BANK;
    if (victim_bank_pos < 2 || victim_bank_pos > ROWS_PER_BANK - 3) {
        // Shift to middle of bank
        uint32_t bank_start_row = (start_row / ROWS_PER_BANK) * ROWS_PER_BANK;
        uint32_t ideal_start = bank_start_row + ROWS_PER_BANK / 2;
        if (ideal_start < min_safe_row) {
            ideal_start = ((min_safe_row / ROWS_PER_BANK) + 1) * ROWS_PER_BANK + ROWS_PER_BANK / 2;
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

            // Select same-bank aggressors
            auto aggr_rows = select_same_bank_aggressors(row, num_sides, min_safe_row);
            uint32_t actual_sides = aggr_rows.size();

            if (actual_sides < 2) {
                fmt::print("   Row {:5d}: skipped (insufficient aggressors in bank)\n", row);
                continue;
            }

            // Convert to addresses
            std::vector<uint32_t> aggr_addrs;
            for (uint32_t r : aggr_rows) {
                aggr_addrs.push_back(r * ROW_SIZE);
            }

            // Print aggressor info on first row
            if (row == start_row) {
                fmt::print("  Aggressors for row {}: [", row);
                for (size_t i = 0; i < aggr_rows.size(); i++) {
                    fmt::print("{}{}", aggr_rows[i], i + 1 < aggr_rows.size() ? ", " : "");
                }
                fmt::print("] ({} rows in bank {}-{})\n",
                           actual_sides,
                           (row / ROWS_PER_BANK) * ROWS_PER_BANK,
                           (row / ROWS_PER_BANK) * ROWS_PER_BANK + ROWS_PER_BANK - 1);
            }

            // ── create program ────────────────────────────────────────
            Program program = CreateProgram();

            KernelHandle kernel_id = CreateKernel(
                program,
                OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_nsided_kernel.cpp",
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
                };
                // Append aggressor addresses (args 10+)
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

            if (num_flips > 0) {
                total_flips_found += num_flips;
                fmt::print("██ ROW {:5d} (0x{:06x}): {} BIT FLIPS! ({} words) "
                           "[{} acts, {} sides, {} cores, {:.2f} M act/s]\n",
                           row, victim_addr, total_bit_flips, num_flips,
                           total_activations, actual_sides, num_cores, act_rate / 1e6);

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
                           "[{} total acts, {} sides, {} cores, {:.2f} M act/s]\n",
                           row, victim_addr, total_activations, actual_sides, num_cores,
                           act_rate / 1e6);
            }

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

    if (total_flips_found > 0) {
        fmt::print("\n  *** ROWHAMMER VULNERABILITY CONFIRMED ***\n");
    } else {
        fmt::print("\n  No bit flips detected. Consider:\n");
        fmt::print("    - Higher --iterations (try 50000000)\n");
        fmt::print("    - Different --start-row to hit other banks\n");
        fmt::print("    - --barrier mode for confirmed activations\n");
        fmt::print("    - Different --pattern (try 0x00000000 or 0xFFFFFFFF)\n");
    }
    fmt::print("═══════════════════════════════════════════════════════════\n");

    mesh_device->close();
    return (total_flips_found > 0) ? 0 : 1;
}
