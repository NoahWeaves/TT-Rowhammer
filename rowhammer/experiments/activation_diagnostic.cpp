// SPDX-FileCopyrightText: © 2026
// SPDX-License-Identifier: Apache-2.0
//
// Activation diagnostic — answers the question: are NOC reads actually causing
// DRAM row activations, or are they being served from the row buffer / coalesced?
//
// Runs 6 micro-benchmarks per channel and prints a table:
//
//   Test 0: SAME_ADDR   — re-read same address (row-buffer hit baseline)
//   Test 1: SAME_ROW    — different column, same row (open-page hit)
//   Test 2: DIFF_ROW    — different row, same bank (row-buffer miss = activation)
//   Test 3: DIFF_BANK   — different bank group
//   Test 4: PIPELINE_AB — pipelined A,B (what the attack does)
//   Test 5: PIPELINE_AA — pipelined A,A (coalesce baseline)
//
// Interpretation:
//   If Test 2 >> Test 0: real row activations are happening (good for attack)
//   If Test 2 ≈ Test 0:  row buffer serving everything (attack is ineffective)
//   If Test 4 >> Test 5: pipelined different-row reads cost more (real activations)
//   If Test 4 ≈ Test 5:  DRAM controller is coalescing pipelined reads

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
static constexpr uint32_t CACHELINE   = 64;
static constexpr uint32_t ROWS_PER_BANK = 16;
static constexpr uint32_t L1_ALIGN    = 64;
static constexpr uint32_t RESULT_WORDS = 16;  // generous

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
    bool all_channels = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--channel") == 0 && i+1 < argc)
            channel_idx = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--all-channels") == 0)
            all_channels = true;
        else if (std::strcmp(argv[i], "--help") == 0) {
            fmt::print("Usage: {} [--channel N] [--all-channels]\n", argv[0]);
            return 0;
        }
    }

    constexpr int device_id = 0;
    auto mesh_device = distributed::MeshDevice::create_unit_mesh(device_id);
    auto& cq = mesh_device->mesh_command_queue();
    IDevice* device = mesh_device->get_devices()[0];
    constexpr CoreCoord worker_core = {1, 2};

    uint32_t l1_base      = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::L1);
    uint32_t scratch_addr = (l1_base + L1_ALIGN - 1) & ~(L1_ALIGN - 1);
    uint32_t result_addr  = (scratch_addr + 256 + L1_ALIGN - 1) & ~(L1_ALIGN - 1);

    uint32_t dram_base    = device->allocator()->get_base_allocator_addr(tt_metal::HalMemType::DRAM);
    // Place test addresses well above allocator base
    uint32_t safe_row     = (dram_base / ROW_SIZE) + 100;
    // Ensure safe_row is in the middle of a bank for clean geometry
    safe_row = ((safe_row / ROWS_PER_BANK) + 1) * ROWS_PER_BANK + ROWS_PER_BANK / 2;

    // Test addresses:
    //   A:     row N, column 0
    //   A_c2:  row N, column 4 (256 bytes offset = different cacheline, same row)
    //   B:     row N+1, column 0 (adjacent row in same bank)
    //   C:     row N+ROWS_PER_BANK, column 0 (next bank group)
    uint32_t addr_a     = safe_row * ROW_SIZE;
    uint32_t addr_a_c2  = safe_row * ROW_SIZE + 4 * CACHELINE;  // same row, col 4
    uint32_t addr_b     = (safe_row + 1) * ROW_SIZE;            // next row, same bank
    uint32_t addr_c     = (safe_row + ROWS_PER_BANK) * ROW_SIZE; // next bank group

    fmt::print("╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║          DRAM Activation Diagnostic                         ║\n");
    fmt::print("╠══════════════════════════════════════════════════════════════╣\n");
    fmt::print("║  Test addresses (safe_row={}):\n", safe_row);
    fmt::print("║    A  = row {:5d} col 0   (0x{:08x})\n", safe_row, addr_a);
    fmt::print("║    A' = row {:5d} col 4   (0x{:08x})  [same row]\n", safe_row, addr_a_c2);
    fmt::print("║    B  = row {:5d} col 0   (0x{:08x})  [diff row, same bank]\n", safe_row+1, addr_b);
    fmt::print("║    C  = row {:5d} col 0   (0x{:08x})  [diff bank group]\n", safe_row+ROWS_PER_BANK, addr_c);
    fmt::print("╚══════════════════════════════════════════════════════════════╝\n\n");

    uint32_t ch_lo = all_channels ? 0 : channel_idx;
    uint32_t ch_hi = all_channels ? 8 : channel_idx + 1;

    fmt::print("Channel       | SAME_ADDR | SAME_ROW | DIFF_ROW | DIFF_BANK | PIPE A,B     | PIPE A,A     | Verdict\n");
    fmt::print("              | (cyc)     | (cyc)    | (cyc)    | (cyc)     | (cyc/pair)   | (cyc/pair)   |\n");
    fmt::print("--------------+-----------+----------+----------+-----------+--------------+--------------+--------\n");

    for (uint32_t cidx = ch_lo; cidx < ch_hi; cidx++) {
        const DramChannel& ch = DRAM_CHANNELS[cidx];

        Program program = CreateProgram();
        KernelHandle kid = CreateKernel(
            program,
            OVERRIDE_KERNEL_PREFIX "rowhammer/kernels/activation_diagnostic_kernel.cpp",
            worker_core,
            DataMovementConfig{
                .processor = DataMovementProcessor::RISCV_0,
                .noc       = NOC::RISCV_0_default});

        SetRuntimeArgs(program, kid, worker_core, {
            ch.noc_x, ch.noc_y,
            scratch_addr, result_addr,
            addr_a, addr_a_c2, addr_b, addr_c,
        });

        distributed::MeshWorkload workload;
        workload.add_program(distributed::MeshCoordinateRange(mesh_device->shape()),
                             std::move(program));
        distributed::EnqueueMeshWorkload(cq, workload, /*blocking=*/false);
        distributed::Finish(cq);

        std::vector<uint32_t> rv;
        detail::ReadFromDeviceL1(device, worker_core, result_addr,
                                 RESULT_WORDS * sizeof(uint32_t), rv);

        uint32_t same_addr  = rv[1];
        uint32_t same_row   = rv[2];
        uint32_t diff_row   = rv[3];
        uint32_t diff_bank  = rv[4];
        uint32_t pipe_ab_t  = rv[5];
        uint32_t pipe_ab_pp = rv[6];
        uint32_t pipe_aa_t  = rv[7];
        uint32_t pipe_aa_pp = rv[8];

        // Verdict: is diff_row significantly slower than same_addr?
        const char* verdict;
        if (diff_row > same_addr * 3 / 2) {
            verdict = "REAL ACTIVATIONS";
        } else if (diff_row > same_addr * 5 / 4) {
            verdict = "MARGINAL";
        } else {
            verdict = "LIKELY COALESCED";
        }

        fmt::print("{:13s} | {:9d} | {:8d} | {:8d} | {:9d} | {:5d} ({:4d}) | {:5d} ({:4d}) | {}\n",
                   ch.name,
                   same_addr, same_row, diff_row, diff_bank,
                   pipe_ab_t, pipe_ab_pp,
                   pipe_aa_t, pipe_aa_pp,
                   verdict);
    }

    fmt::print("\n");
    fmt::print("Interpretation:\n");
    fmt::print("  SAME_ADDR:  row-buffer hit baseline (should be fastest)\n");
    fmt::print("  SAME_ROW:   open-page column access (should be similar to SAME_ADDR)\n");
    fmt::print("  DIFF_ROW:   row-buffer miss = real row activation (should be slowest)\n");
    fmt::print("  DIFF_BANK:  cross-bank-group access\n");
    fmt::print("  PIPE A,B:   pipelined different-row reads (total / per-pair)\n");
    fmt::print("  PIPE A,A:   pipelined same-address reads (total / per-pair)\n");
    fmt::print("\n");
    fmt::print("  If DIFF_ROW >> SAME_ADDR: the attack IS forcing real row activations.\n");
    fmt::print("  If DIFF_ROW ≈ SAME_ADDR:  reads are being served from row buffer.\n");
    fmt::print("  If PIPE A,B >> PIPE A,A:  pipelined cross-row reads cost more (good).\n");
    fmt::print("  If PIPE A,B ≈ PIPE A,A:   DRAM controller coalesces pipelined reads.\n");

    mesh_device->close();
    return 0;
}
