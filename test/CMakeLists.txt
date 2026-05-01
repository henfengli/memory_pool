find_package(Threads REQUIRED)

# Helper function to add a test
function(add_mempool_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE mempool Threads::Threads)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_mempool_test(test_basic)
add_mempool_test(test_arena)
add_mempool_test(test_thread)
add_mempool_test(test_perf)
add_mempool_test(test_perf_detailed)
add_mempool_test(test_reclaim_invariant)
