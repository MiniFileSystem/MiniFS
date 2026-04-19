cmake_minimum_required(VERSION 3.16)
project(nebula_fs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wpointer-arith -Wcast-align
    -Wstrict-prototypes -Wmissing-prototypes
    -D_GNU_SOURCE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -fno-omit-frame-pointer)
else()
    add_compile_options(-O2)
endif()

include_directories(include)

set(NEBULA_CORE_SOURCES
    src/util/crc32c.c
    src/util/uuid.c
    src/util/log.c
    src/io/nebula_io_posix.c
    src/nebula/nebula_layout.c
    src/nebula/nebula_mbr.c
    src/nebula/nebula_superblock.c
    src/nebula/nebula_uberblock.c
    src/nebula/nebula_allocator_roots.c
    src/nebula/nebula_bitmap.c
    src/nebula/nebula_inode_init.c
    src/nebula/nebula_hier_bitmap.c
    src/nebula/nebula_mount.c
    src/nebula/nebula_root_sub_alloc.c
    src/nebula/nebula_block_alloc.c
    src/nebula/nebula_inode_alloc.c
    src/nebula/nebula_file.c
    src/nebula/nebula_dir.c
    src/nebula/nebula_fs.c
)

add_library(nebula_core STATIC ${NEBULA_CORE_SOURCES})
target_include_directories(nebula_core PUBLIC include)

add_executable(nebula_format   tools/nebula_format.c)
add_executable(nebula_dump     tools/nebula_dump.c)
add_executable(nebula_label    tools/nebula_label.c)
add_executable(nebula_fsck     tools/nebula_fsck.c)
add_executable(nebula_discover tools/nebula_discover.c)
add_executable(nebula_mount    tools/nebula_mount.c)
add_executable(nebula_alloc    tools/nebula_alloc.c)
add_executable(nebula_inode    tools/nebula_inode.c)
add_executable(nebula_rw       tools/nebula_rw.c)

foreach(tool nebula_format nebula_dump nebula_label
              nebula_fsck nebula_discover nebula_mount nebula_alloc
              nebula_inode nebula_rw)
    target_link_libraries(${tool} PRIVATE nebula_core)
endforeach()

# ===========================================================================
# Optional SPDK backend (B2-B6)
# Enable with:  cmake -DNEBULA_ENABLE_SPDK=ON -DSPDK_DIR=/path/to/spdk ...
# ===========================================================================
option(NEBULA_ENABLE_SPDK "Build the SPDK NVMe I/O backend" OFF)

if(NEBULA_ENABLE_SPDK)
    if(NOT DEFINED SPDK_DIR)
        message(FATAL_ERROR "NEBULA_ENABLE_SPDK requires -DSPDK_DIR=<path to SPDK build>")
    endif()

    set(SPDK_INCLUDE_DIR "${SPDK_DIR}/include")
    set(SPDK_LIB_DIR     "${SPDK_DIR}/build/lib")

    set(NEBULA_SPDK_SOURCES
        src/io/nebula_spdk_env.c
        src/io/nebula_spdk_nvme.c
        src/io/nebula_io_spdk.c
        src/io/nebula_dma_pool.c
    )

    add_library(nebula_spdk STATIC ${NEBULA_SPDK_SOURCES})
    target_include_directories(nebula_spdk PUBLIC include ${SPDK_INCLUDE_DIR})
    target_compile_definitions(nebula_spdk PUBLIC NEBULA_SPDK_ENABLED)

    # SPDK libraries required for NVMe + environment
    set(SPDK_LIBS
        "${SPDK_LIB_DIR}/libspdk_nvme.a"
        "${SPDK_LIB_DIR}/libspdk_env_dpdk.a"
        "${SPDK_LIB_DIR}/libspdk_log.a"
        "${SPDK_LIB_DIR}/libspdk_util.a"
        "${SPDK_LIB_DIR}/libspdk_sock.a"
    )

    # Probe CLI tool (B6)
    add_executable(nebula_spdk_probe tools/nebula_spdk_probe.c)
    target_include_directories(nebula_spdk_probe PRIVATE include ${SPDK_INCLUDE_DIR})
    target_link_libraries(nebula_spdk_probe PRIVATE
        nebula_core nebula_spdk ${SPDK_LIBS} pthread dl numa)

    message(STATUS "SPDK backend enabled  (SPDK_DIR=${SPDK_DIR})")
else()
    message(STATUS "SPDK backend disabled (pass -DNEBULA_ENABLE_SPDK=ON to enable)")
endif()

message(STATUS "Nebula FS configured: ${CMAKE_BUILD_TYPE}")
