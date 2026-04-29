// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Row-buffer latency probe.
//
// Determines how many distinct *physical* DRAM rows are spanned by our
// nominal "row 10008" address region. Methodology mirrors GPUHammer's
// address-mapping reverse-engineering:
//
//   1. Pick a base address V (warm address that opens a row buffer).
//   2. For each candidate target offset Δ, measure the latency of a single
//      cacheline read at (V + Δ) immediately after warming V.
//   3. Latency clusters into two regimes:
//        - "row hit"  ≈ tCL + NOC ≈ ~30-100 ns  → V+Δ aliases V's row
//        - "row miss" ≈ tRP+tRCD+tCL + NOC ≈ ~100-200 ns → distinct row
//
// The diff between hit and miss is ~tRP+tRCD ≈ 30 ns ≈ 24 BRISC cycles
// at 1.25 ns/cycle. We sample many times per offset and report avg/min/max
// so noise can be averaged out.
//
// Sweep covers cacheline-granularity (within a "row") AND row-granularity
// (across the bank) to catch any address bits that XOR-scramble physical
// rows.

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

static constexpr uint32_t ROW_SIZE        = 8192;
static constexpr uint32_t CACHELINE       = 64;
static constexpr double   NS_PER_CYCLE    = 1.25;
static constexpr uint32_t L1_ALIGN        = 64;
static constexpr uint32_t FLUSH_OFFSET    = 128u * 1024u * 1024u;
static constexpr uint32_t MAX_OFFSETS     = 64;

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

struct OffsetSpec { int32_t bytes; const char* label; };

int main(int argc, char** argv) {
    uint32_t channel_idx     = 0;
    uint32_t sub_port_idx    = 0;
    uint32_t base_row        = 2008;
    uint32_t samples_per_off = 256;

    for (int i = 1; i < argc; i++) {
        if      (!std::strcmp(argv[i], "--channel")  && i+1 < argc) channel_idx  = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sub-port") && i+1 < argc) sub_port_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--base-row") && i+1 < argc) base_row     = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--samples")  && i+1 < argc) samples_per_off = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--help")) {
            fmt::print("Usage: {} [--channel N] [--sub-port N] [--base-row N] [--samples N]\n", argv[0]);
            return 0;
        }
    }
    if (channel_idx >= 8 || sub_port_idx >= 3) return 1;
    const auto& sp = CHANNELS[channel_idx].sp[sub_port_idx];

    // Build the offset sweep.
    // Cacheline-scale: ±1, 2, 4, 8, 16, 32, 64, 128 cachelines  (within a "row"
    //   or just past it). 128 CL = 8 KB = 1 row.
    // Row-scale: ±1, 2, 4, 8, 16, 32 rows  (16 rows = 128 KB = bank stride).
    // Plus a "definite miss" anchor at +128 MB and +256 MB.
    std::vector<OffsetSpec> offsets;
    offsets.push_back({0, "+0 (self)"});
    for (int32_t cl : {1, 2, 4, 8, 16, 32, 64}) {
        offsets.push_back({+cl * (int32_t)CACHELINE, "+CL"});
        offsets.push_back({-cl * (int32_t)CACHELINE, "-CL"});
    }
    for (int32_t r : {1, 2, 4, 8, 15, 16, 17, 18, 32, 64}) {
        offsets.push_back({+r * (int32_t)ROW_SIZE, "+row"});
        offsets.push_back({-r * (int32_t)ROW_SIZE, "-row"});
    }
    offsets.push_back({+(int32_t)FLUSH_OFFSET,         "+128MB"});
    offsets.push_back({+(int32_t)(2 * FLUSH_OFFSET),   "+256MB"});
    if (offsets.size() > MAX_OFFSETS) offsets.resize(MAX_OFFSETS);

    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq  = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];

    uint32_t l1_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result  = (scratch + 4 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_size = MAX_OFFSETS * 3 * sizeof(uint32_t);

    uint32_t dram_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (base_row < min_safe_row) base_row = min_safe_row;
    uint32_t base_addr  = base_row * ROW_SIZE;
    uint32_t flush_addr = base_addr + FLUSH_OFFSET + 17 * ROW_SIZE;  // far away, off-row

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║       Row-Buffer Latency Probe ({}, sub-port {})\n", CHANNELS[channel_idx].name, sub_port_idx);
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Base row:        {} (0x{:08x})\n", base_row, base_addr);
    fmt::print("║  Flush addr:      0x{:08x}  (base+128MB+17 rows)\n", flush_addr);
    fmt::print("║  Offsets tested:  {}\n", offsets.size());
    fmt::print("║  Samples/offset:  {}\n", samples_per_off);
    fmt::print("║  Method: warm base row buffer, then time single read at (base+Δ)\n");
    fmt::print("║          - low avg latency  → Δ aliases base's physical row\n");
    fmt::print("║          - high avg latency → Δ is a distinct physical row (ACT+RD)\n");
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    CoreCoord probe = {0, 0};
    Program prog = CreateProgram();
    KernelHandle kid = CreateKernel(
        prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/row_buffer_latency_kernel.cpp",
        probe,
        DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                            .noc       = NOC::RISCV_0_default });

    std::vector<uint32_t> args = {
        sp.noc_x, sp.noc_y, base_addr,
        scratch, result,
        static_cast<uint32_t>(offsets.size()),
        samples_per_off,
        flush_addr,
    };
    for (const auto& o : offsets) args.push_back(static_cast<uint32_t>(static_cast<int64_t>(base_addr) + o.bytes));
    SetRuntimeArgs(prog, kid, probe, args);

    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);

    std::vector<uint32_t> rv;
    detail::ReadFromDeviceL1(dev, probe, result, result_size, rv);

    // Print per-offset latency in cycles AND nanoseconds, plus a category.
    // We classify by comparing each offset's avg to the median of all "long-distance"
    // probes (offsets ≥ 1 row away), which should always be misses.
    std::vector<uint32_t> miss_samples;
    for (size_t i = 0; i < offsets.size(); i++) {
        if (std::abs(offsets[i].bytes) >= (int32_t)ROW_SIZE)
            miss_samples.push_back(rv[i*3 + 0]);
    }
    std::sort(miss_samples.begin(), miss_samples.end());
    uint32_t miss_median = miss_samples.empty() ? 0 : miss_samples[miss_samples.size()/2];

    // Find a "self" baseline (offset 0) which is the lowest-latency case
    uint32_t self_avg = rv[0];
    uint32_t threshold = (self_avg + miss_median) / 2;

    fmt::print("Self-read (offset 0)        : {} cyc avg   ({:.1f} ns) — LIKELY HIT (warm row buffer)\n",
               self_avg, self_avg * NS_PER_CYCLE);
    fmt::print("Median far-row latency      : {} cyc      ({:.1f} ns) — LIKELY MISS reference\n",
               miss_median, miss_median * NS_PER_CYCLE);
    fmt::print("Hit/miss threshold (mid)    : {} cyc      ({:.1f} ns)\n\n",
               threshold, threshold * NS_PER_CYCLE);

    fmt::print("{:<8} {:<10} {:>10} {:>10} {:>10} {:>10}  category\n",
               "label", "Δ bytes", "avg cyc", "min cyc", "max cyc", "avg ns");
    fmt::print("{:-<78}\n", "");
    for (size_t i = 0; i < offsets.size(); i++) {
        uint32_t avg = rv[i*3 + 0];
        uint32_t mn  = rv[i*3 + 1];
        uint32_t mx  = rv[i*3 + 2];
        const char* cat = (avg < threshold) ? "HIT (same physical row?)"
                                            : "MISS (distinct row)";
        fmt::print("{:<8} {:<+10} {:>10} {:>10} {:>10} {:>10.1f}  {}\n",
                   offsets[i].label, offsets[i].bytes, avg, mn, mx,
                   avg * NS_PER_CYCLE, cat);
    }

    fmt::print("\n[Interpretation]\n");
    fmt::print("  Hit avg   ≈ tCL + NOC overhead (small constant)\n");
    fmt::print("  Miss avg  ≈ tRP + tRCD + tCL + NOC ≈ tRC ≈ 45-50 ns of DRAM time on top of NOC\n");
    fmt::print("  If far rows show HIT, controller is satisfying from row buffer (real ACTs < reads).\n");
    fmt::print("  If +1 row shows HIT, addresses don't map to adjacent physical rows (XOR scrambling).\n");

    mesh->close();
    return 0;
}
