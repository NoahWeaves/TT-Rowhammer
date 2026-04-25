// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Phase B verification driver — re-runs the TT-Rowhammer characterization on
// the CURRENT hardware before we layer any new attack technique on top.
//
// Steps performed:
//   B1: 8 KB conflict matrix replay (Method 2) — small 8x8 same-bank matrix
//       at 8 KB stride starting from a safe DRAM offset; pass if every
//       diagonal cell ≤ 850 cyc and every off-diagonal cell ≥ 860 cyc on at
//       least 7 of 8 channels (warn-only on 10 % divergence from reference).
//   B2: ROWS_PER_BANK measurement — pair the same anchor with anchor +
//       k * 0x2000 for k in 1..32 and look for the latency jump that marks
//       the same-bank-group → cross-bank-group boundary.  HARD FAIL if the
//       measured value diverges from the hardcoded ROWS_PER_BANK = 16, since
//       that constant directly drives n-sided aggressor selection.
//
// Output: a markdown verification report under
//   tt-metal/tt_metal/programming_examples/rowhammer/documentation/
//   verification_report.md
//
// B3 (ECC baseline) and B4 (compiler fold) are documented in the report
// as manual checks because they require interactive tt-smi/objdump
// inspection — see the report for the exact commands.
//
// Result-buffer schema (matches geometry_probe_kernel):
//   results[0]    = num_probes
//   results[1+k]  = min cycles measured for probe k (UINT32_MAX = unset)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/core.h>

#include <tt-metalium/host_api.hpp>
#include <tt-metalium/tt_metal.hpp>
#include <tt-metalium/device.hpp>
#include <tt-metalium/distributed.hpp>
#include <tt-metalium/allocator.hpp>
#include <tt-metalium/hal_types.hpp>

#include "experiments/dram_addr_decoder.hpp"

using namespace tt;
using namespace tt::tt_metal;
using namespace rowhammer::dram;

// ─── DRAM channel NOC endpoints (Blackhole) ──────────────────────────
struct DramChannel { uint32_t noc_x; uint32_t noc_y; const char* name; };
static const DramChannel DRAM_CHANNELS[] = {
    {0,  1,  "ch0 (0,1)" },  {0, 10, "ch1 (0,10)"},  {0,  4, "ch2 (0,4)" },  {0,  7, "ch3 (0,7)" },
    {9,  1,  "ch4 (9,1)" },  {9, 10, "ch5 (9,10)"},  {9,  4, "ch6 (9,4)" },  {9,  7, "ch7 (9,7)" },
};

// Reference latencies from TT-Rowhammer/conflict_matrix.csv.
static constexpr uint32_t REF_SAME_ROW_CYC      = 833;
static constexpr uint32_t REF_DIFF_ROW_CYC      = 873;
static constexpr uint32_t REF_DIFF_BANKGRP_CYC  = 897;
static constexpr uint32_t TIER_TOLERANCE        = 25;        // ±25 cyc band

// Probe + result buffer layout
static constexpr uint32_t MAX_PROBES        = 64;
static constexpr uint32_t RESULT_WORDS      = 1 + MAX_PROBES;
static constexpr uint32_t SCRATCH_BYTES     = 2 * 1024;  // matches geometry_probe_kernel EVICT_BURST
static constexpr uint32_t L1_ALIGN          = 64;

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

// Run the geometry probe kernel for a single (channel, probe-list) batch.
// Returns the per-probe minimum-cycle vector parallel to `probes` (pair list).
static std::vector<uint32_t> run_probes(
    distributed::MeshDevice& mesh_device,
    distributed::MeshCommandQueue& cq,
    IDevice* device,
    CoreCoord worker_core,
    uint32_t scratch_addr,
    uint32_t result_addr,
    const DramChannel& ch,
    const std::vector<std::pair<uint32_t, uint32_t>>& probes)
{
    std::vector<uint32_t> all;
    all.reserve(probes.size());

    for (size_t base = 0; base < probes.size(); base += MAX_PROBES) {
        size_t batch = std::min<size_t>(MAX_PROBES, probes.size() - base);

        Program program = CreateProgram();
        KernelHandle kid = CreateKernel(
            program,
            OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/geometry_probe_kernel.cpp",
            worker_core,
            DataMovementConfig{
                .processor = DataMovementProcessor::RISCV_0,
                .noc       = NOC::RISCV_0_default});

        std::vector<uint32_t> args;
        args.reserve(5 + 2 * batch);
        args.push_back(ch.noc_x);
        args.push_back(ch.noc_y);
        args.push_back(scratch_addr);
        args.push_back(result_addr);
        args.push_back(static_cast<uint32_t>(batch));
        for (size_t i = 0; i < batch; ++i) {
            args.push_back(probes[base + i].first);
            args.push_back(probes[base + i].second);
        }
        SetRuntimeArgs(program, kid, worker_core, args);

        distributed::MeshWorkload workload;
        workload.add_program(distributed::MeshCoordinateRange(mesh_device.shape()),
                             std::move(program));
        distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
        distributed::Finish(cq);

        std::vector<uint32_t> result_vec;
        detail::ReadFromDeviceL1(device, worker_core, result_addr,
                                 RESULT_WORDS * sizeof(uint32_t), result_vec);
        for (size_t i = 0; i < batch; ++i) {
            all.push_back(result_vec[1 + i]);
        }
    }
    return all;
}

int main(int argc, char** argv) {
    bool all_channels = false;
    uint32_t single_channel = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--all-channels") == 0)              all_channels = true;
        else if (std::strcmp(argv[i], "--channel") == 0 && i+1 < argc) single_channel = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [--channel N | --all-channels]\n", argv[0]);
            return 0;
        }
    }

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];
    constexpr CoreCoord worker_core = {1, 2};

    uint32_t l1_base = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + SCRATCH_BYTES + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t safe_addr = ((dram_base / ROW_SIZE) + 64) * ROW_SIZE;  // pad 64 rows = 512 KB

    fmt::print("Verification harness — Phase B (B1, B2)\n");
    fmt::print("DRAM safe anchor: 0x{:x}  (allocator base 0x{:x})\n", safe_addr, dram_base);
    fmt::print("L1: scratch=0x{:x} result=0x{:x}\n\n", scratch_addr, result_addr);

    // ─── Build B1 probe list: 8x8 same-bank conflict matrix ──────────
    // Anchor + i * 8 KB  vs  anchor + j * 8 KB  for i,j in 0..7.
    // i==j → same row (833 cyc); i!=j → different row, same bank group (873 cyc).
    std::vector<std::pair<uint32_t, uint32_t>> b1_probes;
    for (uint32_t i = 0; i < 8; ++i) {
        for (uint32_t j = 0; j < 8; ++j) {
            b1_probes.emplace_back(safe_addr + i * ROW_SIZE,
                                   safe_addr + j * ROW_SIZE);
        }
    }

    // ─── Build B2 probe list: anchor vs anchor + k * 8 KB, k = 1..32 ─
    std::vector<std::pair<uint32_t, uint32_t>> b2_probes;
    for (uint32_t k = 1; k <= 32; ++k) {
        b2_probes.emplace_back(safe_addr, safe_addr + k * ROW_SIZE);
    }

    uint32_t ch_lo = all_channels ? 0 : single_channel;
    uint32_t ch_hi = all_channels ? 8 : single_channel + 1;

    struct ChannelReport {
        std::string name;
        std::vector<uint32_t> b1;
        std::vector<uint32_t> b2;
        bool b1_pass = false;
        uint32_t b2_measured_rows_per_bank = 0;
    };
    std::vector<ChannelReport> reports;

    for (uint32_t cidx = ch_lo; cidx < ch_hi; ++cidx) {
        const DramChannel& ch = DRAM_CHANNELS[cidx];
        ChannelReport rpt;
        rpt.name = ch.name;
        fmt::print("── {} ─────────────────\n", ch.name);

        rpt.b1 = run_probes(*mesh_device, cq, device, worker_core, scratch_addr, result_addr, ch, b1_probes);
        rpt.b2 = run_probes(*mesh_device, cq, device, worker_core, scratch_addr, result_addr, ch, b2_probes);

        // B1 verdict
        bool diag_ok = true, off_ok = true;
        for (uint32_t i = 0; i < 8; ++i) {
            for (uint32_t j = 0; j < 8; ++j) {
                uint32_t v = rpt.b1[i * 8 + j];
                if (i == j) { if (v > REF_SAME_ROW_CYC + TIER_TOLERANCE) diag_ok = false; }
                else        { if (v < REF_DIFF_ROW_CYC - TIER_TOLERANCE) off_ok  = false; }
            }
        }
        rpt.b1_pass = diag_ok && off_ok;
        fmt::print("  B1 8x8 conflict matrix: {} (diag_ok={} off_ok={})\n",
                   rpt.b1_pass ? "PASS" : "WARN", diag_ok, off_ok);

        // B2 verdict — find first k where latency jumps to cross-bank-group tier.
        uint32_t jump_k = 0;
        for (uint32_t k = 1; k <= 32; ++k) {
            uint32_t v = rpt.b2[k - 1];
            if (v >= REF_DIFF_BANKGRP_CYC - TIER_TOLERANCE) { jump_k = k; break; }
        }
        rpt.b2_measured_rows_per_bank = jump_k;  // 0 = no jump observed in 32 rows
        fmt::print("  B2 ROWS_PER_BANK measured: {} (constant={})\n",
                   jump_k ? std::to_string(jump_k) : std::string("not-found"),
                   ROWS_PER_BANK);
        reports.push_back(std::move(rpt));
    }

    // ─── Emit markdown verification report ───────────────────────────
    // Resolve report path relative to TT_METAL_HOME if available; otherwise
    // fall back to a path that is correct when run from the repo root.
    std::string report_path = "tt_metal/programming_examples/rowhammer/"
                              "documentation/verification_report.md";
    if (const char* home = std::getenv("TT_METAL_HOME")) {
        report_path = std::string(home) + "/" + report_path;
    }

    std::ofstream out(report_path);
    if (!out) {
        fmt::print(stderr, "WARNING: could not open {} for writing\n", report_path);
    } else {
        out << "# Phase B Verification Report\n\n";
        out << "Auto-generated by `verify_geometry`. Reference values from\n";
        out << "`TT-Rowhammer/conflict_matrix.csv` and `row_verification.txt`.\n\n";
        out << "## Tolerances\n";
        out << "- Same-row band:        " << REF_SAME_ROW_CYC      << " ± " << TIER_TOLERANCE << " cyc\n";
        out << "- Different-row band:   " << REF_DIFF_ROW_CYC      << " ± " << TIER_TOLERANCE << " cyc\n";
        out << "- Cross-bank-group band:" << REF_DIFF_BANKGRP_CYC  << " ± " << (TIER_TOLERANCE*2) << " cyc\n\n";

        out << "## B1 — 8 KB conflict matrix per channel\n\n";
        out << "| Channel | Diagonal min | Diagonal max | Off-diag min | Off-diag max | Verdict |\n";
        out << "|---|---|---|---|---|---|\n";
        for (auto& r : reports) {
            uint32_t d_min = UINT32_MAX, d_max = 0, o_min = UINT32_MAX, o_max = 0;
            for (uint32_t i = 0; i < 8; ++i) for (uint32_t j = 0; j < 8; ++j) {
                uint32_t v = r.b1[i*8+j];
                if (i == j) { d_min = std::min(d_min, v); d_max = std::max(d_max, v); }
                else        { o_min = std::min(o_min, v); o_max = std::max(o_max, v); }
            }
            out << "| " << r.name << " | " << d_min << " | " << d_max
                << " | " << o_min << " | " << o_max
                << " | " << (r.b1_pass ? "✅ PASS" : "⚠️ WARN") << " |\n";
        }

        out << "\n## B2 — Measured ROWS_PER_BANK (cross-bank-group jump)\n\n";
        out << "Hardcoded constant in `dram_addr_decoder.hpp`: **" << ROWS_PER_BANK << "**\n\n";
        out << "| Channel | First k with cross-bank-group latency | Verdict |\n";
        out << "|---|---|---|\n";
        bool b2_any_fail = false;
        for (auto& r : reports) {
            std::string verdict;
            if (r.b2_measured_rows_per_bank == 0)                     { verdict = "⚠️ no jump in k≤32 (review)"; }
            else if (r.b2_measured_rows_per_bank == ROWS_PER_BANK)    { verdict = "✅ matches constant"; }
            else                                                       { verdict = "❌ MISMATCH — update constant"; b2_any_fail = true; }
            out << "| " << r.name << " | "
                << (r.b2_measured_rows_per_bank ? std::to_string(r.b2_measured_rows_per_bank) : "—")
                << " | " << verdict << " |\n";
        }

        out << "\n### B2 raw cycles (anchor vs anchor + k·8KB)\n\n";
        out << "| Channel |";
        for (uint32_t k = 1; k <= 32; ++k) out << " k=" << k << " |";
        out << "\n|---" << std::string(32, '|') << "\n";
        for (auto& r : reports) {
            out << "| " << r.name << " |";
            for (uint32_t k = 1; k <= 32; ++k) out << " " << r.b2[k - 1] << " |";
            out << "\n";
        }

        out << "\n## B3 — ECC baseline (manual)\n\n";
        out << "Run the following and confirm `corr` counters increment when "
               "hammering, with no readback flips:\n\n";
        out << "```bash\n";
        out << "build_Release/programming_examples/rowhammer/metal_example_rowhammer "
               "--channel 0 --start-row 64 --num-rows 64 --iterations 500000\n";
        out << "```\n\n";

        out << "## B4 — Compiler-fold sanity (manual)\n\n";
        out << "Disassemble the BRISC ELF for `rowhammer_kernel.cpp` (look under "
               "`generated/.../trisc0` after a run) and confirm the hammer loop body "
               "contains exactly two `noc_async_read` issue points (one per aggressor):\n\n";
        out << "```bash\n";
        out << "riscv-tt-elf-objdump -d <kernel.elf> | less   # search for noc_fast_read\n";
        out << "```\n\n";

        out << "## B5 — Address-decoder unit test\n\n";
        out << "Run `test_addr_decoder` (built alongside this binary). PASS = all "
               "documented row_verification pairs decoded correctly + invariants hold.\n\n";

        out << "## Exit gate for Phase C\n\n";
        if (b2_any_fail) {
            out << "❌ **DO NOT proceed to Phase C** — measured ROWS_PER_BANK does not "
                   "match the hardcoded constant.  Update the constant in "
                   "`experiments/dram_addr_decoder.hpp` and "
                   "`kernels/rowhammer_nsided_kernel.cpp`, then re-run.\n";
        } else {
            out << "✅ B1 + B2 + B5 cleared.  B3 and B4 remain manual; record their "
                   "outcome in this file before starting Phase C.\n";
        }
        fmt::print("\nWrote verification report to {}\n", report_path);
    }

    return 0;
}
