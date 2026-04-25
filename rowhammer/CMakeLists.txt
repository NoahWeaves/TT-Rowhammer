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
