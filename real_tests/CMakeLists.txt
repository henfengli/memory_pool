find_package(Threads REQUIRED)

add_library(real_tests_common STATIC bench_common.cpp)
target_include_directories(real_tests_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

function(add_real_bench name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE mempool real_tests_common Threads::Threads)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

add_real_bench(bench_pattern_single)
add_real_bench(bench_pattern_multi)
add_real_bench(bench_pattern_xthread)
add_real_bench(bench_fastpath)
add_real_bench(bench_scenario_game)
add_real_bench(bench_scenario_network)
add_real_bench(bench_scenario_pipeline)
add_real_bench(bench_pattern_overhead)
add_real_bench(probe_xthread_cliff)
add_real_bench(probe_pagefault)
add_real_bench(probe_k_sweep)
add_real_bench(perf_focused)
add_real_bench(perf_t8)
