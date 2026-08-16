cmake_minimum_required(VERSION 3.21)
project(hazptr_proto CXX)

if(MSVC)
  # MSVC has no /std:c++26 flag; /std:c++latest covers the latest draft.
  # Don't set CMAKE_CXX_STANDARD: CMake would fail propagating an unknown CXX26
  # dialect to all targets (including external packages like googletest).
  add_compile_options(/std:c++latest)
else()
  set(CMAKE_CXX_STANDARD 26)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SANITIZE_THREAD   "Build with ThreadSanitizer"   OFF)
option(SANITIZE_ADDRESS  "Build with AddressSanitizer"  OFF)
option(WARNINGS_AS_ERRORS "Treat warnings as errors"    OFF)
option(ENABLE_CLANG_TIDY  "Run clang-tidy on project targets" OFF)
option(ENABLE_BENCHMARKS  "Build the microbenchmarks (bench/)"          OFF)

if(ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
endif()

# CPM is vendored (cmake/CPM.cmake, v0.40.2, upstream release verbatim --
# sha256 c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d).
# It used to be fetched per configure with file(DOWNLOAD), which had no STATUS
# or EXPECTED_HASH: a throttled release-CDN response wrote an empty file,
# include() then succeeded silently, and configure died on "Unknown CMake
# command CPMAddPackage" -- an error naming neither the download nor the cause.
# To update, drop in the new release file and refresh the version and hash here.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/CPM.cmake")

CPMAddPackage("gh:google/googletest@1.15.2")
# gtest 1.15.2 has an implicit char8_t→char32_t conversion that triggers
# -Wcharacter-conversion in C++26 mode. Suppress it on gtest targets only.
foreach(_gt gtest gtest_main gmock gmock_main)
  if(TARGET ${_gt})
    target_compile_options(${_gt} PRIVATE
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-character-conversion>)
  endif()
endforeach()

enable_testing()
add_subdirectory(tests)

if(ENABLE_BENCHMARKS)
  add_subdirectory(bench)
endif()
