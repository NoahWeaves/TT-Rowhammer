// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Row-buffer cache depth probe.
//
// Sweeps N = number of distinct rows cycled through in pipelined burst mode.
// For each N, measures cyc/read. Compares to:
//   - same-row (N=1) pipelined floor: ~48 cyc/read
//   - cross-row pipelined penalty: ~57 cyc/read
//
// The smallest N that produces the penalty = (controller row-buffer cache
// depth + 1) = minimum number of aggressor rows needed for guaranteed real
// DRAM ACTs. For rowhammer, this is the smallest hammer-set that defeats
// the controller's caching, maximizing per-row activation pressure.

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

struct Result { uint32_t n; uint32_t reads; uint32_t niu_req; uint32_t niu_resp; double cyc_per_read; double mr_per_sec; };

static Result run_n(distributed::MeshDevice* mesh, IDevice* dev,
                    const SubPort& sp, uint32_t base_addr, uint32_t iterations,
                    uint32_t n_rows, uint32_t scratch, uint32_t result, uint32_t result_size,
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
                   {sp.noc_x, sp.noc_y, base_addr, iterations, /*mode*/7u,
                    scratch, result, n_rows});

    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);

    std::vector<uint32_t> rv;
    detail::ReadFromDeviceL1(dev, probe, result, result_size, rv);
    uint64_t cycles = (static_cast<uint64_t>(rv[1]) << 32) | rv[0];
    uint32_t reads  = rv[2];
    double sec = cycles * NS_PER_CYCLE / 1e9;
    return {n_rows, reads, rv[4], rv[5],
            reads ? double(cycles) / reads : 0,
            reads ? reads / sec / 1e6 : 0};
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
    CoreCoord probe = {0, 0};

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Row-Buffer Cache Depth Probe (mode 7, K=16 pipelined)\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Channel:        {} sub-port {} (NOC {},{})\n",
               CHANNELS[channel_idx].name, sub_port_idx, sp.noc_x, sp.noc_y);
    fmt::print("║  Base addr:      0x{:08x}  (row {})\n", base_addr, base_row);
    fmt::print("║  Iterations:     {} per N\n", iterations);
    fmt::print("║  Reference: same-row pipelined ≈ 48 cyc, cross-row ≈ 57 cyc\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    fmt::print("{:>4} {:>10} {:>10} {:>10} {:>14} {:>12}  {}\n",
               "N", "reads", "NIU req", "NIU resp", "cyc/read (ns)", "M reads/s",
               "interpretation");
    fmt::print("{:-<110}\n", "");

    static const uint32_t N_VALUES[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 24, 32, 48, 64};
    double base_cyc = 0;
    bool printed_knee = false;

    for (uint32_t n : N_VALUES) {
        Result r = run_n(mesh.get(), dev, sp, base_addr, iterations,
                         n, scratch, result, result_size, probe);
        if (n == 1) base_cyc = r.cyc_per_read;
        const char* note;
        double delta = r.cyc_per_read - base_cyc;
        if (delta < 2.0)              note = "all hits (cached)";
        else if (delta < 6.0)         note = "mixed";
        else if (!printed_knee)     { note = "<— KNEE: cache exceeded"; printed_knee = true; }
        else                          note = "all misses (uncached)";
        fmt::print("{:>4} {:>10} {:>10} {:>10} {:>10.1f} ({:>4.0f}) {:>12.2f}  {}\n",
                   r.n, r.reads, r.niu_req, r.niu_resp,
                   r.cyc_per_read, r.cyc_per_read * NS_PER_CYCLE,
                   r.mr_per_sec, note);
    }

    fmt::print("\n[Interpretation]\n");
    fmt::print("  N=1 floor = same-row pipelined ceiling (NOC + row buffer hit).\n");
    fmt::print("  When cyc/read jumps by ≥4-6 cyc, the controller's row buffer cache\n");
    fmt::print("  is overflowed. That N is the smallest hammer-set that produces real\n");
    fmt::print("  ACTs at the GDDR6 controller. For rowhammer, per-row ACT rate is\n");
    fmt::print("  bounded above by (single-thread DRAM throughput / N).\n");

    mesh->close();
    return 0;
}
