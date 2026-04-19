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
)

add_library(nebula_core STATIC ${NEBULA_CORE_SOURCES})
target_include_directories(nebula_core PUBLIC include)

add_executable(nebula_format tools/nebula_format.c)
target_link_libraries(nebula_format PRIVATE nebula_core)

add_executable(nebula_dump tools/nebula_dump.c)
target_link_libraries(nebula_dump PRIVATE nebula_core)

message(STATUS "Nebula FS configured: ${CMAKE_BUILD_TYPE}")
