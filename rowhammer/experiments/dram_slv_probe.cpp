// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// DRAM-side NIU slave-counter probe.
//
// Brackets a known-N reads kernel between two snapshots of the DRAM tile's
// NIU_SLV_RD_REQ_RECEIVED counter (read via NOC from a worker tile, into
// L1, then host-read). Reports kernel-claimed reads vs DRAM-side observed.
//
// Pipeline:
//   1. Launch snapshot kernel → reads NIU_SLV at (dram_x, dram_y) into L1
//   2. Host reads pre value from L1
//   3. Launch access-pattern kernel → does N NOC reads to DRAM addr
//   4. Launch snapshot kernel again → reads NIU_SLV again into L1
//   5. Host reads post value, computes delta, subtracts known overhead.
//
// The snapshot reads themselves contribute +1 each to the SLV counter.
// We launch 2 snapshots so the delta = N (kernel) + 1 (post-snapshot read,
// which IS counted) - 0 (pre-snapshot read happened before the pre-value
// was captured into L1, so its +1 is already in the pre-value).
// Adjustment: subtract 1 from the delta to remove the post-snapshot overhead.
//
// Verifies whether N kernel reads → N+ packets received at DRAM NIU.

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

static constexpr uint32_t ROW_SIZE      = 8192;
static constexpr uint32_t CACHELINE     = 64;
static constexpr uint32_t L1_ALIGN      = 64;

// NIU SLV counter MMIO address (per blackhole noc_parameters.h):
// NOC_REGS_START_ADDR(0xFFB20000) + 0x200 + 0x35*4 = 0xFFB202D4
static constexpr uint32_t NIU_SLV_RD_REQ_RECEIVED_ADDR = 0xFFB20000 + 0x200 + 0x35 * 4;

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

static uint32_t snapshot_niu_slv(distributed::MeshDevice* mesh, IDevice* dev,
                                 const SubPort& sp, CoreCoord probe,
                                 uint32_t l1_dst_addr)
{
    auto& cq = mesh->mesh_command_queue();
    Program p = CreateProgram();
    KernelHandle k = CreateKernel(
        p, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/noc_register_snapshot_kernel.cpp",
        probe,
        DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                            .noc       = NOC::RISCV_0_default });
    SetRuntimeArgs(p, k, probe,
                   {sp.noc_x, sp.noc_y, NIU_SLV_RD_REQ_RECEIVED_ADDR, l1_dst_addr});
    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(p));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);

    std::vector<uint32_t> rv;
    detail::ReadFromDeviceL1(dev, probe, l1_dst_addr, 4, rv);
    return rv[0];
}

struct ProbeResult { uint32_t mode; uint32_t kernel_reads; uint32_t niu_mst; uint32_t slv_delta; };

static ProbeResult run_pattern(distributed::MeshDevice* mesh, IDevice* dev,
                               const SubPort& sp, uint32_t base_addr, uint32_t iterations,
                               uint32_t mode, uint32_t scratch, uint32_t result, uint32_t result_size,
                               uint32_t snap_l1, CoreCoord probe)
{
    auto& cq = mesh->mesh_command_queue();

    // Pre-snapshot via probe core
    uint32_t pre = snapshot_niu_slv(mesh, dev, sp, probe, snap_l1);

    // Hammer kernel
    Program prog = CreateProgram();
    KernelHandle kid = CreateKernel(
        prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/access_pattern_kernel.cpp",
        probe,
        DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                            .noc       = NOC::RISCV_0_default });
    SetRuntimeArgs(prog, kid, probe,
                   {sp.noc_x, sp.noc_y, base_addr, iterations, mode, scratch, result});
    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);

    // Read kernel result
    std::vector<uint32_t> rv;
    detail::ReadFromDeviceL1(dev, probe, result, result_size, rv);
    uint32_t kernel_reads = rv[2];
    uint32_t niu_mst      = rv[4];

    // Post-snapshot
    uint32_t post = snapshot_niu_slv(mesh, dev, sp, probe, snap_l1);

    // Delta minus the post-snapshot's own +1 contribution.
    // (The pre-snapshot also contributed +1 but that increment landed BEFORE
    // we captured pre, so it's already in pre. The post-snapshot read happens
    // AFTER the snapshot kernel reads the register but before we capture
    // post — so its +1 may or may not be included depending on hardware
    // ordering. We subtract 1 to be conservative.)
    uint32_t raw_delta = (post - pre) & 0xFFFFFFFF;
    uint32_t adjusted  = raw_delta > 0 ? raw_delta - 1 : 0;

    return {mode, kernel_reads, niu_mst, adjusted};
}

int main(int argc, char** argv) {
    uint32_t channel_idx  = 0;
    uint32_t sub_port_idx = 0;
    uint32_t base_row     = 2008;
    uint32_t iterations   = 40000;

    for (int i = 1; i < argc; i++) {
        if      (!std::strcmp(argv[i], "--channel")    && i+1 < argc) channel_idx  = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sub-port")   && i+1 < argc) sub_port_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--base-row")   && i+1 < argc) base_row     = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--iterations") && i+1 < argc) iterations   = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--sub-port N] [--base-row N] [--iterations N]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8 || sub_port_idx >= 3) return 1;
    const auto& sp = CHANNELS[channel_idx].sp[sub_port_idx];

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    IDevice* dev = mesh->get_devices()[0];

    uint32_t l1_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result  = (scratch + 8 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t snap_l1 = (result  + 8 * sizeof(uint32_t) + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_size = 8 * sizeof(uint32_t);

    uint32_t dram_base    = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (base_row < min_safe_row) base_row = min_safe_row;
    uint32_t base_addr = base_row * ROW_SIZE;
    CoreCoord probe = {0, 0};

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       DRAM-side NIU Slave Counter Probe\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Target:         {} sub-port {} (NOC {},{})\n",
               CHANNELS[channel_idx].name, sub_port_idx, sp.noc_x, sp.noc_y);
    fmt::print("║  Base addr:      0x{:08x}  (row {})\n", base_addr, base_row);
    fmt::print("║  Iterations:     {} per mode\n", iterations);
    fmt::print("║  Probe core:     logical (0,0) on Tensix grid\n");
    fmt::print("║  Counter:        NIU_SLV_RD_REQ_RECEIVED at 0x{:08x}\n", NIU_SLV_RD_REQ_RECEIVED_ADDR);
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    struct ModeSpec { uint32_t mode; const char* name; };
    static const ModeSpec MODES[5] = {
        {0, "single_addr_noFlush"},
        {1, "adjacent_CL_noFlush"},
        {2, "next_row_8KB_noFlush"},
        {3, "FLUSH_READ"},
        {4, "4way_128MB_far"},
    };

    fmt::print("{:<3} {:<24} {:>10} {:>10} {:>14} {:>10}  {}\n",
               "M", "name", "kernel rd", "NIU MST", "DRAM SLV Δ", "ratio", "interpretation");
    fmt::print("{:-<110}\n", "");

    for (const auto& m : MODES) {
        ProbeResult r = run_pattern(mesh.get(), dev, sp, base_addr, iterations,
                                    m.mode, scratch, result, result_size, snap_l1, probe);
        double ratio = r.kernel_reads ? double(r.slv_delta) / r.kernel_reads : 0;
        const char* note;
        if (ratio > 0.95)       note = "every NOC read reaches DRAM tile";
        else if (ratio > 0.50)  note = "partial — NOC fabric merging?";
        else if (ratio > 0.10)  note = "heavy merging at fabric";
        else                    note = "DRAM tile receives almost nothing";
        fmt::print("{:<3} {:<24} {:>10} {:>10} {:>14} {:>10.2f}  {}\n",
                   m.mode, m.name, r.kernel_reads, r.niu_mst, r.slv_delta, ratio, note);
    }

    fmt::print("\n[Interpretation guide]\n");
    fmt::print("  ratio ≈ 1.0  → kernel reads = NOC packets sent = packets received at DRAM tile\n");
    fmt::print("                 (BUT may still hit row buffer at controller — not = real ACTs)\n");
    fmt::print("  ratio < 1.0  → NOC fabric is merging/dropping reads upstream of DRAM\n");
    fmt::print("                 (would mean our 'M act/s' overstates DRAM-tile traffic)\n");

    mesh->close();
    return 0;
}
