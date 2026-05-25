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

if(ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
endif()

# Download CPM on first configure
set(CPM_VERSION 0.40.2)
set(CPM_CACHE "${CMAKE_CURRENT_BINARY_DIR}/CPM_${CPM_VERSION}.cmake")
if(NOT EXISTS "${CPM_CACHE}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"
    "${CPM_CACHE}"
  )
endif()
include("${CPM_CACHE}")

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
