// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Sub-port aliasing probe.
//
// Writes a unique pattern to victim_addr via sub-port 0 of a channel, then
// reads the same victim_addr via all 3 sub-ports and prints what each saw.
// Distinguishes "sub-ports alias to same physical bank" (all 3 reads match)
// from "sub-ports map to distinct memory regions" (only sp0 matches).

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

static constexpr uint32_t ROW_SIZE   = 8192;
static constexpr uint32_t CACHELINE  = 64;
static constexpr uint32_t L1_ALIGN   = 64;

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

int main(int argc, char** argv) {
    uint32_t channel_idx = 0;
    uint32_t victim_row  = 2048;
    uint32_t pattern     = 0xdeadbeef;

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--channel") && i+1 < argc) channel_idx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--row") && i+1 < argc) victim_row = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pattern") && i+1 < argc) pattern = std::strtoul(argv[++i], nullptr, 0);
    }

    const auto& ch = CHANNELS[channel_idx];
    constexpr int device_id = 0;
    auto mesh = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq  = mesh->mesh_command_queue();
    IDevice* dev = mesh->get_devices()[0];
    CoreCoord worker = {0, 0};

    uint32_t l1_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result  = (scratch + 4 * CACHELINE + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base = dev->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    uint32_t min_safe_row = (dram_base / ROW_SIZE) + 2;
    if (victim_row < min_safe_row) victim_row = min_safe_row;
    uint32_t victim_addr = victim_row * ROW_SIZE;

    fmt::print("Sub-port aliasing probe\n");
    fmt::print("  Channel:   {}  sub-ports: ({},{}) ({},{}) ({},{})\n",
               ch.name, ch.sp[0].noc_x, ch.sp[0].noc_y,
               ch.sp[1].noc_x, ch.sp[1].noc_y, ch.sp[2].noc_x, ch.sp[2].noc_y);
    fmt::print("  Victim:    row {} (0x{:08x})\n", victim_row, victim_addr);
    fmt::print("  Pattern:   0x{:08x}\n\n", pattern);

    Program prog = CreateProgram();
    KernelHandle kid = CreateKernel(
        prog, std::string(OVERRIDE_KERNEL_PREFIX) + "rowhammer/kernels/subport_probe_kernel.cpp",
        worker,
        DataMovementConfig{ .processor = DataMovementProcessor::RISCV_0,
                            .noc       = NOC::RISCV_0_default });
    SetRuntimeArgs(prog, kid, worker, {
        ch.sp[0].noc_x, ch.sp[0].noc_y,
        ch.sp[1].noc_x, ch.sp[1].noc_y,
        ch.sp[2].noc_x, ch.sp[2].noc_y,
        victim_addr, pattern, scratch, result
    });

    distributed::MeshWorkload wl;
    wl.add_program(distributed::MeshCoordinateRange(mesh->shape()), std::move(prog));
    distributed::EnqueueMeshWorkload(cq, wl, false);
    distributed::Finish(cq);

    std::vector<uint32_t> rv;
    detail::ReadFromDeviceL1(dev, worker, result, 4 * sizeof(uint32_t), rv);
    fmt::print("After write of 0x{:08x} via sub-port 0, reads at the same victim_addr:\n", pattern);
    fmt::print("  via sub-port 0 ({},{}):  0x{:08x}  {}\n",
               ch.sp[0].noc_x, ch.sp[0].noc_y, rv[0],
               rv[0] == pattern ? "MATCH" : "diff");
    fmt::print("  via sub-port 1 ({},{}):  0x{:08x}  {}\n",
               ch.sp[1].noc_x, ch.sp[1].noc_y, rv[1],
               rv[1] == pattern ? "MATCH" : "diff");
    fmt::print("  via sub-port 2 ({},{}):  0x{:08x}  {}\n",
               ch.sp[2].noc_x, ch.sp[2].noc_y, rv[2],
               rv[2] == pattern ? "MATCH" : "diff");

    int matches = (rv[0] == pattern) + (rv[1] == pattern) + (rv[2] == pattern);
    fmt::print("\nVerdict: ");
    if (matches == 3)
        fmt::print("ALL THREE sub-ports alias to the same physical bank.\n"
                   "  → sub-port scaling is real per-bank parallelism.\n");
    else if (matches == 1 && rv[0] == pattern)
        fmt::print("Only sub-port 0 matches → sub-ports map to distinct memory regions.\n"
                   "  → sub-port scaling is per-region scaling, not per-bank.\n");
    else
        fmt::print("Mixed result ({} matches) — partial aliasing or interleaving.\n", matches);

    mesh->close();
    return 0;
}
