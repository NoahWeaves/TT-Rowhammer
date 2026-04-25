// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Rowhammer host program for Tenstorrent Blackhole GDDR6.
//
// This program sets up the device, launches the rowhammer kernel on a chosen
// Tensix worker core, and reads back results.  It sweeps across a configurable
// range of victim rows and reports any bit flips found.
//
// Usage:
//   ./metal_example_rowhammer [options]
//
// The program prints a summary of all tested rows and any detected bit flips,
// plus timing/activation-rate statistics.

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
// We shell out to Python to read the GDDR6 ECC telemetry counters.
// This is the only reliable way to detect corrected (single-bit) flips
// that the on-die ECC silently fixes before data reaches the NOC.
//
// Returns: {gddr01_corr, gddr23_corr, gddr45_corr, gddr67_corr, uncorr}
//          All values are cumulative counters (monotonically increasing).
//          Returns {UINT32_MAX, ...} on failure.

struct EccCounters {
    uint32_t corr[4];    // correctable errors per channel pair: 01, 23, 45, 67
    uint32_t uncorr;     // uncorrectable errors (all channels)
    bool valid;

    uint32_t total_corr() const { return corr[0] + corr[1] + corr[2] + corr[3]; }
};

static EccCounters read_ecc_counters() {
    EccCounters ec{};
    ec.valid = false;

    // Python one-liner that prints 5 space-separated integers
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
        if (parsed == 5) {
            ec.valid = true;
        }
    }
    pclose(pipe);
    return ec;
}

static void print_ecc_delta(const char* label, const EccCounters& before, const EccCounters& after) {
    if (!before.valid || !after.valid) {
        fmt::print("  ECC {}: counters unavailable (pyluwen not accessible)\n", label);
        return;
    }
    uint32_t d01 = after.corr[0] - before.corr[0];
    uint32_t d23 = after.corr[1] - before.corr[1];
    uint32_t d45 = after.corr[2] - before.corr[2];
    uint32_t d67 = after.corr[3] - before.corr[3];
    uint32_t du  = after.uncorr  - before.uncorr;
    uint32_t total_corr = d01 + d23 + d45 + d67;

    if (total_corr > 0 || du > 0) {
        fmt::print("  ⚡ ECC {}: +{} corrected (ch01:{} ch23:{} ch45:{} ch67:{}), +{} uncorrectable\n",
                   label, total_corr, d01, d23, d45, d67, du);
        if (total_corr > 0) {
            fmt::print("    → ECC is SILENTLY CORRECTING bit flips — rowhammer IS inducing errors!\n");
        }
        if (du > 0) {
            fmt::print("    → UNCORRECTABLE errors detected — multi-bit flips exceeded ECC!\n");
        }
    } else {
        fmt::print("  ECC {}: no new corrected or uncorrectable errors\n", label);
    }
}

// ─── constants from TT-Rowhammer characterisation ─────────────────────
static constexpr uint32_t ROW_SIZE           = 8192;       // 8 KB per DRAM row
static constexpr uint32_t CACHELINE          = 64;         // bytes per NOC transaction
static constexpr double   NS_PER_CYCLE       = 1.25;      // 1 / 800 MHz (BRISC clock)

// Result buffer layout (must match kernel)
static constexpr uint32_t RESULT_HDR_WORDS   = 6;
static constexpr uint32_t MAX_FLIP_RECORDS   = 32;
static constexpr uint32_t FLIP_RECORD_WORDS  = 4;
static constexpr uint32_t RESULT_BUF_WORDS   = RESULT_HDR_WORDS + MAX_FLIP_RECORDS * FLIP_RECORD_WORDS;

// ─── Blackhole DRAM channel 0 NOC coordinates ─────────────────────────
// From blackhole_140_arch.yaml:
//   Channel 0 endpoints: [0-0, 0-1, 0-11]
//   The worker_endpoint for channel 0 is subchannel index 2 → 0-11,
//   but sub-channel 1 (0,1) was validated in the characterisation work.
// We use the dram_views worker_endpoint which resolves to the second
// subchannel: NOC (0,1).  All three sub-ports access the same physical
// GDDR6 channel, so the choice is functionally equivalent.
//
// The DRAM address space per channel is ~4 GB (dram_bank_size = 4278190080).
// We must stay within this range and avoid addresses used by the allocator.
// We use high addresses (starting at 256 MB = 0x10000000) to avoid
// collisions with tt-metal's own allocations which start at the bottom.

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

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

int main(int argc, char* argv[]) {
    // ─── configuration ────────────────────────────────────────────────
    // These can be made into command-line arguments later.
    uint32_t channel_idx       = 0;                     // DRAM channel to attack
    uint32_t start_row         = 32;                    // first victim row number (skip row 0-31 for safety)
    uint32_t num_rows_to_test  = 64;                    // how many victim rows to sweep
    uint32_t hammer_iterations = 500000;                // iterations per row (each = 2 activations = 1M acts)
    uint32_t data_pattern      = 0x55555555;            // victim data pattern
    bool     all_channels      = false;                 // sweep all 8 channels
    uint32_t use_barrier       = 0;                     // 0 = pipelined, 1 = serialized

    // Parse simple command-line overrides
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
        } else if (std::strcmp(argv[i], "--all-channels") == 0) {
            all_channels = true;
        } else if (std::strcmp(argv[i], "--barrier") == 0) {
            use_barrier = 1;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [options]\n", argv[0]);
            fmt::print("  --channel N       DRAM channel index 0-7 (default: 0)\n");
            fmt::print("  --start-row N     First victim row number (default: 32)\n");
            fmt::print("  --num-rows N      Number of rows to sweep (default: 64)\n");
            fmt::print("  --iterations N    Hammer iterations per row (default: 500000)\n");
            fmt::print("  --pattern 0xNN    Victim data pattern (default: 0x55555555)\n");
            fmt::print("  --all-channels    Sweep all 8 DRAM channels\n");
            fmt::print("  --barrier         Use serialized reads (slower but confirmed activations)\n");
            return 0;
        }
    }

    if (channel_idx >= 8) {
        fmt::print(stderr, "Error: channel must be 0-7\n");
        return 1;
    }

    // Ensure victim rows have valid aggressors (need row-1 and row+1)
    if (start_row < 1) {
        start_row = 1;
    }

    const DramChannel& ch = DRAM_CHANNELS[channel_idx];
    uint32_t total_activations_per_row = hammer_iterations * 2;

    // Determine channels to sweep
    uint32_t ch_start = all_channels ? 0 : channel_idx;
    uint32_t ch_end   = all_channels ? 8 : channel_idx + 1;

    fmt::print("╔══════════════════════════════════════════════════════════╗\n");
    fmt::print("║     Blackhole GDDR6 Rowhammer Test                     ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════╣\n");
    if (all_channels) {
        fmt::print("║  Channels:     ALL (0-7)                   \n");
    } else {
        fmt::print("║  Channel:      {} ({})                \n", channel_idx, ch.name);
        fmt::print("║  DRAM NOC:     ({}, {})                \n", ch.noc_x, ch.noc_y);
    }
    fmt::print("║  Victim rows:  {} to {}                \n", start_row, start_row + num_rows_to_test - 1);
    fmt::print("║  Iterations:   {} ({} activations/row) \n", hammer_iterations, total_activations_per_row);
    fmt::print("║  Pattern:      0x{:08x}  (aggr: 0x{:08x})  \n", data_pattern, ~data_pattern);
    fmt::print("║  Mode:         {}                      \n", use_barrier ? "serialized (barrier)" : "pipelined (fast)");
    fmt::print("║  Row size:     {} bytes (8 KB)         \n", ROW_SIZE);
    fmt::print("╚══════════════════════════════════════════════════════════╝\n\n");

    // ─── read ECC counters before test ────────────────────────────────
    fmt::print("Reading ECC counters (before)...\n");
    EccCounters ecc_before = read_ecc_counters();
    if (ecc_before.valid) {
        fmt::print("  corr: ch01={} ch23={} ch45={} ch67={}  uncorr={}\n",
                   ecc_before.corr[0], ecc_before.corr[1],
                   ecc_before.corr[2], ecc_before.corr[3], ecc_before.uncorr);
    } else {
        fmt::print("  WARNING: Could not read ECC counters. Install pyluwen in PATH.\n");
        fmt::print("  Continuing without ECC monitoring.\n");
    }
    fmt::print("\n");

    // ─── device setup ─────────────────────────────────────────────────
    constexpr int device_id = 0;
    std::shared_ptr<distributed::MeshDevice> mesh_device =
        distributed::MeshDevice::create_unit_mesh(device_id);
    distributed::MeshCommandQueue& cq = mesh_device->mesh_command_queue();

    // We'll use Tensix worker core (1,2) — the first functional worker on Blackhole.
    constexpr CoreCoord worker_core = {1, 2};

    // ─── allocate L1 buffers for scratch and results ──────────────────
    // We need:
    //   - 128 bytes scratch (two 64-byte cache lines for NOC read destinations)
    //   - RESULT_BUF_WORDS * 4 bytes for results
    // We lay these out manually at the L1 base allocator address on the
    // worker core, like mem_bench does.  This ensures the addresses are on
    // the same core where the kernel runs.
    constexpr uint32_t scratch_size = 2 * CACHELINE;  // 128 bytes
    constexpr uint32_t result_size  = RESULT_BUF_WORDS * sizeof(uint32_t);

    IDevice* device = mesh_device->get_devices()[0];
    uint32_t l1_base = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    // Align to 64 bytes (NOC transaction size)
    constexpr uint32_t L1_ALIGN = 64;
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + scratch_size + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    fmt::print("L1 layout: base=0x{:x}  scratch=0x{:x}  result=0x{:x}\n",
               l1_base, scratch_addr, result_addr);

    // ─── DRAM reserved region check ───────────────────────────────────
    // Query the DRAM allocator base to find where free DRAM starts.
    // Our victim rows and their neighbors (row-1, row+1) must all be
    // above this base to avoid corrupting firmware/profiler data.
    uint32_t dram_base = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    // Need row-1 to be above dram_base, so min victim row = (dram_base / ROW_SIZE) + 2
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    fmt::print("DRAM reserved: 0x0 - 0x{:x}  (allocator base)\n", dram_base);
    fmt::print("Min safe victim row: {} (addr 0x{:x}, aggressor_lo at 0x{:x})\n",
               min_safe_row, min_safe_row * ROW_SIZE, (min_safe_row - 1) * ROW_SIZE);

    if (start_row < min_safe_row) {
        fmt::print("WARNING: --start-row {} is in reserved DRAM! Auto-adjusting to {}\n",
                   start_row, min_safe_row);
        start_row = min_safe_row;
    }
    fmt::print("\n");

    // ─── tracking ─────────────────────────────────────────────────────
    uint32_t total_flips_found = 0;
    uint32_t rows_tested = 0;
    uint32_t ecc_corr_total = 0;
    uint32_t ecc_uncorr_total = 0;
    EccCounters ecc_batch_start = ecc_before;  // track deltas per batch

    // ─── sweep victim rows ────────────────────────────────────────────
    for (uint32_t ch_idx = ch_start; ch_idx < ch_end; ch_idx++) {
    const DramChannel& cur_ch = DRAM_CHANNELS[ch_idx];
    if (all_channels) {
        fmt::print("\n── Channel {} ({}) ──────────────────────────────────\n",
                   ch_idx, cur_ch.name);
    }

    for (uint32_t row = start_row; row < start_row + num_rows_to_test; row++) {
        uint32_t victim_addr = row * ROW_SIZE;

        // Create a fresh program for each row (programs are lightweight)
        Program program = CreateProgram();

        KernelHandle kernel_id = CreateKernel(
            program,
            OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/rowhammer_kernel.cpp",
            worker_core,
            DataMovementConfig{
                .processor = DataMovementProcessor::RISCV_0,
                .noc = NOC::RISCV_0_default});

        SetRuntimeArgs(
            program,
            kernel_id,
            worker_core,
            {
                cur_ch.noc_x,               // arg 0: DRAM NOC X
                cur_ch.noc_y,               // arg 1: DRAM NOC Y
                victim_addr,                // arg 2: victim row base address
                hammer_iterations,          // arg 3: number of hammer iterations
                data_pattern,               // arg 4: data pattern
                scratch_addr,               // arg 5: L1 scratch address
                result_addr,                // arg 6: L1 result buffer address
                use_barrier,                // arg 7: 0 = pipelined, 1 = serialized
            });

        // Execute
        distributed::MeshWorkload workload;
        distributed::MeshCoordinateRange device_range =
            distributed::MeshCoordinateRange(mesh_device->shape());
        workload.add_program(device_range, std::move(program));
        distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
        distributed::Finish(cq);

        // Read back results from L1 on the specific worker core
        std::vector<uint32_t> result_vec;
        detail::ReadFromDeviceL1(device, worker_core, result_addr, result_size, result_vec);

        // Parse results
        [[maybe_unused]] uint32_t status = result_vec[0];
        uint32_t num_flips       = result_vec[1];
        uint32_t total_bit_flips = result_vec[2];
        uint32_t cycles_lo       = result_vec[3];
        uint32_t cycles_hi       = result_vec[4];
        uint32_t activations     = result_vec[5];
        uint64_t cycles          = (static_cast<uint64_t>(cycles_hi) << 32) | cycles_lo;
        double   elapsed_us      = cycles * NS_PER_CYCLE / 1000.0;
        double   act_rate        = (activations > 0 && cycles > 0)
                                     ? (static_cast<double>(activations) / (cycles * NS_PER_CYCLE / 1e9))
                                     : 0.0;

        rows_tested++;

        if (num_flips > 0) {
            total_flips_found += num_flips;
            fmt::print("██ ROW {:5d} (0x{:06x}): {} BIT FLIPS DETECTED! ({} words affected)\n",
                       row, victim_addr, total_bit_flips, num_flips);
            fmt::print("   Hammer: {} activations in {:.1f} μs ({:.2f} M act/s)\n",
                       activations, elapsed_us, act_rate / 1e6);

            // Print flip details
            uint32_t records = (num_flips < MAX_FLIP_RECORDS) ? num_flips : MAX_FLIP_RECORDS;
            for (uint32_t r = 0; r < records; r++) {
                uint32_t base = RESULT_HDR_WORDS + r * FLIP_RECORD_WORDS;
                uint32_t cl_idx   = result_vec[base + 0];
                uint32_t word_off = result_vec[base + 1];
                uint32_t expected = result_vec[base + 2];
                uint32_t actual   = result_vec[base + 3];
                uint32_t diff     = expected ^ actual;

                uint32_t byte_offset = cl_idx * CACHELINE + word_off * sizeof(uint32_t);
                fmt::print("   Flip #{}: row {} offset 0x{:04x} (CL {} word {}): "
                           "expected 0x{:08x}, got 0x{:08x}, diff 0x{:08x}\n",
                           r + 1, row, byte_offset, cl_idx, word_off,
                           expected, actual, diff);
            }
            fmt::print("\n");
        } else {
            // Brief progress line (overwrite for clean output)
            if (rows_tested % 8 == 0 || row == start_row) {
                fmt::print("   Row {:5d} (0x{:06x}): no flips  [{} activations, {:.2f} M act/s]\n",
                           row, victim_addr, activations, act_rate / 1e6);
            }
        }

        // ─── periodic ECC check (every 8 rows) ───────────────────────
        if (rows_tested % 8 == 0) {
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
                    ecc_corr_total += batch_corr;
                    ecc_uncorr_total += batch_uncorr;
                }
                ecc_batch_start = ecc_now;
            }
        }
    }
    } // end channel loop

    // ─── final ECC counter read ─────────────────────────────────────
    fmt::print("\nReading ECC counters (after)...\n");
    EccCounters ecc_after = read_ecc_counters();
    if (ecc_after.valid) {
        fmt::print("  corr: ch01={} ch23={} ch45={} ch67={}  uncorr={}\n",
                   ecc_after.corr[0], ecc_after.corr[1],
                   ecc_after.corr[2], ecc_after.corr[3], ecc_after.uncorr);
    }

    // Accumulate any remaining delta not caught by periodic checks
    if (ecc_after.valid && ecc_batch_start.valid) {
        uint32_t remaining_corr =
            (ecc_after.corr[0] - ecc_batch_start.corr[0]) +
            (ecc_after.corr[1] - ecc_batch_start.corr[1]) +
            (ecc_after.corr[2] - ecc_batch_start.corr[2]) +
            (ecc_after.corr[3] - ecc_batch_start.corr[3]);
        uint32_t remaining_uncorr = ecc_after.uncorr - ecc_batch_start.uncorr;
        ecc_corr_total += remaining_corr;
        ecc_uncorr_total += remaining_uncorr;
    }

    // ─── summary ──────────────────────────────────────────────────────
    fmt::print("\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("  SUMMARY\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("  Rows tested:       {}\n", rows_tested);
    fmt::print("  Total bit flips:   {} (uncorrected, visible in readback)\n", total_flips_found);
    fmt::print("  Acts per row:      {}\n", total_activations_per_row);
    fmt::print("  Pattern:           0x{:08x}\n", data_pattern);
    fmt::print("  Channel:           {}\n", all_channels ? "ALL (0-7)" : fmt::format("{} ({})", channel_idx, ch.name));
    fmt::print("───────────────────────────────────────────────────────────\n");

    // ECC summary
    if (ecc_before.valid && ecc_after.valid) {
        print_ecc_delta("total", ecc_before, ecc_after);
        fmt::print("  ECC corrected Δ:   {}\n", ecc_corr_total);
        fmt::print("  ECC uncorrectable: {}\n", ecc_uncorr_total);

        if (ecc_corr_total > 0 && total_flips_found == 0) {
            fmt::print("\n  *** ECC IS MASKING ROWHAMMER BIT FLIPS ***\n");
            fmt::print("  The attack IS causing physical bit flips, but on-die ECC\n");
            fmt::print("  is correcting them before they reach the NOC readback.\n");
            fmt::print("  Try N-sided hammering to induce multi-bit errors.\n");
        } else if (ecc_corr_total > 0 && total_flips_found > 0) {
            fmt::print("\n  *** ROWHAMMER VULNERABILITY CONFIRMED ***\n");
            fmt::print("  Both corrected AND uncorrected flips detected.\n");
        }
    } else {
        fmt::print("  ECC monitoring:    unavailable (pyluwen not in PATH)\n");
    }

    if (total_flips_found > 0) {
        fmt::print("\n  *** ROWHAMMER VULNERABILITY CONFIRMED (UNCORRECTED FLIPS) ***\n");
    } else if (ecc_corr_total == 0) {
        fmt::print("\n  No bit flips detected (neither visible nor ECC-corrected). Consider:\n");
        fmt::print("    - Increasing --iterations (try 5000000 for 10M activations)\n");
        fmt::print("    - Testing more rows with --num-rows 512\n");
        fmt::print("    - Trying complementary pattern --pattern 0xAAAAAAAA\n");
        fmt::print("    - Trying a different channel with --channel N\n");
    }
    fmt::print("═══════════════════════════════════════════════════════════\n");

    mesh_device->close();
    return (total_flips_found > 0) ? 0 : 1;
}
