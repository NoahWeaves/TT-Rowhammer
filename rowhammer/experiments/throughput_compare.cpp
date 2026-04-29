// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Throughput-comparison probe.
//
// Determines whether our FLUSH-READ "ground truth" pattern is actually
// causing real DRAM ACTs, vs being satisfied from the row buffer.
//
// Method: run the access-pattern kernel on a SINGLE core in 5 modes, each
// for K iterations, and measure cycles. Throughput = reads / time.
//
// Expected ordering if FLUSH-READ is real (every read = ACT):
//   Mode 0 (single addr, no flush)  ≥ Mode 1 (adjacent CL) ≫ Mode 3 (FLUSH-READ)
//   Mode 4 (4-way far) should be similar to Mode 3 (every read = miss)
//   Mode 2 (one row apart) should be between 1 and 3
//
// If the controller has a deep row buffer / multi-row cache, OR if FLUSH-READ
// fails to close the target row buffer, we will see Mode 3 ≈ Mode 0 — meaning
// FLUSH does NOTHING and our "real activation" counts have been wrong.

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
static constexpr double   NS_PER_CYCLE  = 1.25;
static constexpr uint32_t L1_ALIGN      = 64;

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

struct ModeSpec { uint32_t mode; const char* name; const char* expectation; };
static const ModeSpec MODES[7] = {
    {0, "single_addr_barriered",    "barriered: NOC RT dominates per read"},
    {1, "adjacent_CL_barriered",    "barriered: NOC RT dominates per read"},
    {2, "next_row_8KB_barriered",   "barriered: NOC RT dominates per read"},
    {3, "FLUSH_READ_barriered",     "barriered: NOC RT dominates per read"},
    {4, "4way_128MB_barriered",     "barriered: NOC RT dominates per read"},
    {5, "single_addr_pipelined16",  "pipelined: row hit ~48 cyc/read expected"},
    {6, "FLUSH_READ_pipelined16",   "pipelined: if FLUSH works, ~50ns extra/read"},
};

struct Result { double cyc_per_read; double mr_per_sec; uint64_t cycles; uint32_t reads; uint32_t niu_req; uint32_t niu_resp; };

static Result run_mode(distributed::MeshDevice* mesh, IDevice* dev,
                       const SubPort& sp, uint32_t base_addr, uint32_t iterations,
                       uint32_t mode, uint32_t scratch, uint32_t result, uint32_t result_size,
                       CoreCoord probe)
{
    auto& cq = mesh->mesh_command_queue();
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

    std::vector<uint32_t> rv;
    detail::ReadFromDeviceL1(dev, probe, result, result_size, rv);
    uint64_t cycles = (static_cast<uint64_t>(rv[1]) << 32) | rv[0];
    uint32_t reads  = rv[2];
    uint32_t niu_req  = rv[4];
    uint32_t niu_resp = rv[5];
    double sec = cycles * NS_PER_CYCLE / 1e9;
    return Result{
        reads ? double(cycles) / reads : 0,
        reads ? reads / sec / 1e6      : 0,
        cycles, reads, niu_req, niu_resp,
    };
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
    uint32_t result_size = 8 * sizeof(uint32_t);

    uint32_t dram_base    = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (base_row < min_safe_row) base_row = min_safe_row;
    uint32_t base_addr = base_row * ROW_SIZE;

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Access-Pattern Throughput Comparison\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:        {} sub-port {}\n", CHANNELS[channel_idx].name, sub_port_idx);
    fmt::print("║  Base addr:      0x{:08x}  (row {})\n", base_addr, base_row);
    fmt::print("║  Iterations:     {} (per mode)\n", iterations);
    fmt::print("║  Single core, single RISC, sequential issue with barrier per read\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    CoreCoord probe = {0, 0};
    fmt::print("{:<3} {:<28} {:>10} {:>10} {:>10} {:>14} {:>12}  {}\n",
               "M", "name", "reads", "NIU req", "NIU resp", "cyc/read (ns)",
               "M reads/s", "expectation");
    fmt::print("{:-<118}\n", "");

    std::vector<Result> results;
    for (const auto& m : MODES) {
        Result r = run_mode(mesh.get(), dev, sp, base_addr, iterations,
                            m.mode, scratch, result, result_size, probe);
        results.push_back(r);
        fmt::print("{:<3} {:<28} {:>10} {:>10} {:>10} {:>10.1f} ({:>4.0f}) {:>12.2f}  {}\n",
                   m.mode, m.name, r.reads, r.niu_req, r.niu_resp, r.cyc_per_read,
                   r.cyc_per_read * NS_PER_CYCLE, r.mr_per_sec, m.expectation);
    }

    fmt::print("\n[Interpretation]\n");
    double mode0 = results[0].cyc_per_read;
    double mode3 = results[3].cyc_per_read;
    double mode4 = results[4].cyc_per_read;

    if (mode3 < mode0 * 1.1) {
        fmt::print("  ❗ Mode 3 (FLUSH-READ) ≈ Mode 0 (no FLUSH) → FLUSH does NOT increase per-access cost.\n");
        fmt::print("    Either: (a) NOC dominates and tRC is invisible, or\n");
        fmt::print("            (b) FLUSH is satisfied without closing target row buffer.\n");
        fmt::print("    Implication: our 'real activations' count is upper-bound, not ground truth.\n");
    } else {
        fmt::print("  ✓ Mode 3 (FLUSH-READ) is {:.1f}× slower than Mode 0 → FLUSH adds real ACT cost.\n",
                   mode3 / mode0);
    }
    if (mode4 < mode3 * 1.1 && mode4 > mode3 * 0.9) {
        fmt::print("  ✓ Mode 3 ≈ Mode 4 (4-way far) → FLUSH-READ matches definite-miss throughput.\n");
    } else if (mode4 > mode3 * 1.1) {
        fmt::print("  ⚠ Mode 4 (4-way) is slower than FLUSH-READ → FLUSH may not always miss.\n");
    } else {
        fmt::print("  ⚠ Mode 4 (4-way) is faster than FLUSH-READ → unexpected.\n");
    }

    mesh->close();
    return 0;
}
