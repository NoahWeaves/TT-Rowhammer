cmake_minimum_required(VERSION 3.22...4.2)
project(metal_example_rowhammer)

if(NOT TARGET TT::Metalium)
    find_package(TT-Metalium REQUIRED)
endif()

# ── Helper to register a TT-Metalium host program target. ────────────
# include_dirs is forwarded as PRIVATE so kernel-side headers don't leak.
function(rh_add_host name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PUBLIC TT::Metalium)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(TARGET TT::CommonPCH)
        target_precompile_headers(${name} REUSE_FROM TT::CommonPCH)
    endif()
endfunction()

# ── Existing baselines ───────────────────────────────────────────────
rh_add_host(metal_example_rowhammer        rowhammer.cpp)
rh_add_host(metal_example_rowhammer_nsided rowhammer_nsided.cpp)

# ── Phase B verification ─────────────────────────────────────────────
rh_add_host(metal_example_rh_verify_geometry  experiments/verify_geometry.cpp)

# B5 unit test — pure host code, no TT::Metalium dependency.
add_executable(metal_example_rh_test_addr_decoder experiments/test_addr_decoder.cpp)
target_include_directories(metal_example_rh_test_addr_decoder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ── Phase C new techniques ───────────────────────────────────────────
rh_add_host(metal_example_rh_find_trefi       experiments/find_trefi.cpp)
rh_add_host(metal_example_rh_pattern_sweep    experiments/pattern_sweep.cpp)
rh_add_host(metal_example_rh_build_row_set    experiments/build_row_set.cpp)
rh_add_host(metal_example_rh_write_hammer     experiments/write_hammer.cpp)
rh_add_host(metal_example_rh_write_hammer_multi experiments/write_hammer_multi.cpp)
rh_add_host(metal_example_rh_activation_diag   experiments/activation_diagnostic.cpp)
rh_add_host(metal_example_rh_buffer_diag      experiments/buffer_diagnostic.cpp)
rh_add_host(metal_example_rh_forced_act       experiments/forced_activation_test.cpp)
rh_add_host(metal_example_rh_bank_probe      experiments/bank_conflict_probe.cpp)
rh_add_host(metal_example_rh_core_scaling    experiments/core_scaling_sweep.cpp)
rh_add_host(metal_example_rh_bank_scaling    experiments/bank_scaling_sweep.cpp)
rh_add_host(metal_example_rh_peak_attack     experiments/peak_attack.cpp)
rh_add_host(metal_example_rh_subport_sweep   experiments/subport_sweep.cpp)
rh_add_host(metal_example_rh_subport_probe   experiments/subport_probe.cpp)
rh_add_host(metal_example_rh_intra_channel   experiments/intra_channel_sweep.cpp)
rh_add_host(metal_example_rh_peak_refsync    experiments/peak_attack_refsync.cpp)
rh_add_host(metal_example_rh_bank_boundary   experiments/bank_boundary_probe.cpp)
rh_add_host(metal_example_rh_blacksmith      experiments/peak_attack_blacksmith.cpp)
rh_add_host(metal_example_rh_row_latency     experiments/row_buffer_latency.cpp)
rh_add_host(metal_example_rh_throughput      experiments/throughput_compare.cpp)
rh_add_host(metal_example_rh_slv_probe       experiments/dram_slv_probe.cpp)
rh_add_host(metal_example_rh_row_cycle       experiments/row_cycle_sweep.cpp)
rh_add_host(metal_example_rh_long_hammer     experiments/long_hammer.cpp)
