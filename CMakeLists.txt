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
)

add_library(nebula_core STATIC ${NEBULA_CORE_SOURCES})
target_include_directories(nebula_core PUBLIC include)

add_executable(nebula_format   tools/nebula_format.c)
add_executable(nebula_dump     tools/nebula_dump.c)
add_executable(nebula_label    tools/nebula_label.c)
add_executable(nebula_fsck     tools/nebula_fsck.c)
add_executable(nebula_discover tools/nebula_discover.c)
add_executable(nebula_mount    tools/nebula_mount.c)

foreach(tool nebula_format nebula_dump nebula_label
              nebula_fsck nebula_discover nebula_mount)
    target_link_libraries(${tool} PRIVATE nebula_core)
endforeach()

message(STATUS "Nebula FS configured: ${CMAKE_BUILD_TYPE}")
