cmake_minimum_required(VERSION 3.14)
project(mempool VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Put all binaries in the same directory so tests can find the DLL
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(MEMPOOL_BUILD_TESTS "Build tests" OFF)
option(MEMPOOL_DEBUG "Enable debug fill and checks" OFF)
option(MEMPOOL_STATS "Enable per-alloc statistics tracking" OFF)

# --- Library ---
set(MEMPOOL_SOURCES
    src/arena.cpp
    src/tlc.cpp
    src/stats.cpp
    src/mempool.cpp
)

if(WIN32)
    list(APPEND MEMPOOL_SOURCES src/platform_win32.cpp)
else()
    list(APPEND MEMPOOL_SOURCES src/platform_linux.cpp)
endif()

add_library(mempool SHARED ${MEMPOOL_SOURCES})
target_include_directories(mempool
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(mempool PRIVATE MEMPOOL_BUILDING)
if(MEMPOOL_DEBUG)
    target_compile_definitions(mempool PUBLIC MEMPOOL_DEBUG=1)
endif()
if(MEMPOOL_STATS)
    target_compile_definitions(mempool PUBLIC MEMPOOL_STATS=1)
endif()

if(NOT WIN32)
    target_link_libraries(mempool PRIVATE pthread)
    set_target_properties(mempool PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )
endif()

# Hide all symbols by default, only export MP_API
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mempool PRIVATE -fvisibility=hidden)
endif()

# --- Tests ---
if(MEMPOOL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# --- Examples ---
add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE mempool)
