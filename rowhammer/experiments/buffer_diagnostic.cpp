// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Buffer layer diagnostic host driver — identifies what's absorbing
// row-switch penalties between the NOC and DRAM cells.

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

static constexpr uint32_t ROW_SIZE    = 8192;
static constexpr uint32_t L1_ALIGN    = 64;
static constexpr uint32_t RESULT_WORDS = 64;

struct DramChannel { uint32_t noc_x; uint32_t noc_y; const char* name; };
static const DramChannel DRAM_CHANNELS[] = {
    {0,1,"ch0 (0,1)"},{0,10,"ch1 (0,10)"},{0,4,"ch2 (0,4)"},{0,7,"ch3 (0,7)"},
    {9,1,"ch4 (9,1)"},{9,10,"ch5 (9,10)"},{9,4,"ch6 (9,4)"},{9,7,"ch7 (9,7)"},
};

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

int main(int argc, char** argv) {
    uint32_t channel_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--channel") == 0 && i+1 < argc)
            channel_idx = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [--channel N]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8) return 1;
    const DramChannel& ch = DRAM_CHANNELS[channel_idx];

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];
    constexpr CoreCoord worker_core = {1, 2};

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + 512 + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    // Need 8MB+ headroom above base for stride sweep
    uint32_t safe_row     = (dram_base / ROW_SIZE) + 128;
    uint32_t base_addr    = safe_row * ROW_SIZE;

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║          Buffer Layer Diagnostic                            ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:    {} ({},{})\n", ch.name, ch.noc_x, ch.noc_y);
    fmt::print("║  Base addr:  0x{:08x} (row {})\n", base_addr, safe_row);
    fmt::print("║  Headroom:   ~8 MB above base\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    Program program = CreateProgram();
    KernelHandle kid = CreateKernel(
        program,
        OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/buffer_diagnostic_kernel.cpp",
        worker_core,
        DataMovementConfig{
            .processor = DataMovementProcessor::RISCV_0,
            .noc       = NOC::RISCV_0_default});

    SetRuntimeArgs(program, kid, worker_core, {
        ch.noc_x, ch.noc_y, scratch_addr, result_addr, base_addr,
    });

    distributed::MeshWorkload workload;
    workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                         std::move(program));
    distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
    distributed::Finish(cq);

    std::vector<uint32_t> rv;
    detail::ReadFromDeviceL1(device, worker_core, result_addr,
                             RESULT_WORDS * sizeof(uint32_t), rv);

    // ── Section A: Data Provenance ──────────────────────────────────
    fmt::print("═══ SECTION A: Data Provenance ═══════════════════════════════\n");
    fmt::print("  Write unique patterns to 16 rows, read back, verify.\n");
    fmt::print("  Tests whether reads actually return DRAM content.\n\n");

    uint32_t num_tested  = rv[17];
    uint32_t num_correct = rv[18];
    for (uint32_t r = 0; r < 16; r++) {
        fmt::print("    Row {:2d} (0x{:08x}): pattern 0x{:08x} → {}\n",
                   r, base_addr + r * ROW_SIZE, (r+1) * 0x01010101u,
                   rv[1+r] ? "CORRECT" : "WRONG");
    }
    fmt::print("\n  Result: {}/{} rows returned correct data\n", num_correct, num_tested);
    if (num_correct == num_tested) {
        fmt::print("  → Reads DO reach DRAM. The buffering is hiding latency, not data.\n");
        fmt::print("  → The DRAM controller serves reads without visible row-switch cost.\n");
    } else {
        fmt::print("  → Some reads returned WRONG data — a cache is serving stale content!\n");
    }

    // ── Section B: Row Buffer Saturation ────────────────────────────
    fmt::print("\n═══ SECTION B: Row Buffer Saturation ═════════════════════════\n");
    fmt::print("  Open K rows (K=1..16), then re-access the first row.\n");
    fmt::print("  Latency jump reveals how many row buffers the controller has.\n\n");
    fmt::print("    K  | re-access cyc | delta from K=1\n");
    fmt::print("   ----+---------------+---------------\n");
    uint32_t baseline_b = rv[19];  // K=1
    for (uint32_t K = 1; K <= 16; K++) {
        uint32_t cyc = rv[19 + (K-1)];
        int32_t delta = static_cast<int32_t>(cyc) - static_cast<int32_t>(baseline_b);
        const char* flag = "";
        if (K > 1 && delta > 50) flag = " <── EVICTION?";
        fmt::print("   {:2d}  | {:13d} | {:+5d}{}\n", K, cyc, delta, flag);
    }

    // ── Section C: Large Stride Sweep ───────────────────────────────
    fmt::print("\n═══ SECTION C: Large Stride Sweep ════════════════════════════\n");
    fmt::print("  Time (base, base+stride) for stride = 8KB..8MB.\n");
    fmt::print("  Finds the real bank-conflict stride if address model is wrong.\n\n");
    fmt::print("    stride      | re-access cyc | note\n");
    fmt::print("   -------------+---------------+-----\n");
    uint32_t baseline_c = rv[35];  // stride = 8KB
    for (uint32_t s = 0; s < 16; s++) {
        uint32_t stride = ROW_SIZE << s;
        if (stride > 8 * 1024 * 1024) break;
        uint32_t cyc = rv[35 + s];
        if (cyc == 0xFFFFFFFF) continue;

        const char* unit = "KB";
        uint32_t display = stride / 1024;
        if (display >= 1024) { display /= 1024; unit = "MB"; }

        int32_t delta = static_cast<int32_t>(cyc) - static_cast<int32_t>(baseline_c);
        const char* flag = "";
        if (delta > 50) flag = " <── DIFFERENT TIER";
        else if (delta < -50) flag = " <── FASTER";

        fmt::print("   {:5d} {:2s}     | {:13d} | {:+5d}{}\n",
                   display, unit, cyc, delta, flag);
    }

    // ── Section D: Write-Read vs Read-Read ──────────────────────────
    fmt::print("\n═══ SECTION D: Write-Read vs Read-Read ═══════════════════════\n");
    uint32_t rr_cyc = rv[51];
    uint32_t wr_cyc = rv[52];
    fmt::print("  Read B then read A:    {} cyc\n", rr_cyc);
    fmt::print("  Write B then read A:   {} cyc\n", wr_cyc);
    if (wr_cyc > rr_cyc * 5 / 4) {
        fmt::print("  → Writes force a REAL row activation that reads don't.\n");
        fmt::print("  → Write-based hammering should be used for the attack.\n");
    } else {
        fmt::print("  → Write and read paths show similar latency.\n");
    }

    // ── Summary ─────────────────────────────────────────────────────
    fmt::print("\n═══ SUMMARY ══════════════════════════════════════════════════\n");
    if (num_correct == num_tested) {
        fmt::print("  Reads reach DRAM (data provenance confirmed).\n");
    }
    fmt::print("  All serialized read latencies: ~{} cyc (no row-switch penalty visible).\n", baseline_b);
    fmt::print("  Pipelined throughput: ~60 cyc/pair (from activation_diagnostic).\n\n");

    // Check if any section B entry shows a jump
    bool found_eviction = false;
    for (uint32_t K = 2; K <= 16; K++) {
        int32_t delta = static_cast<int32_t>(rv[19+(K-1)]) - static_cast<int32_t>(baseline_b);
        if (delta > 50) { found_eviction = true; break; }
    }
    if (!found_eviction) {
        fmt::print("  No row-buffer eviction detected up to K=16.\n");
        fmt::print("  Possible explanations:\n");
        fmt::print("    1. GDDR6 controller has >= 16 row buffers (one per bank)\n");
        fmt::print("       and consecutive 8KB rows map to DIFFERENT banks.\n");
        fmt::print("    2. An intermediate cache (L2/NOC) absorbs all reads.\n");
        fmt::print("    3. The controller reorders/pipelines across bank groups.\n");
    }

    mesh_device->close();
    return 0;
}
