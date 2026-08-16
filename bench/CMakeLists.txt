# Microbenchmarks for review point 3.  Opt-in (ENABLE_BENCHMARKS=ON, or the
# `bench` preset) so that CI, the sanitizer presets and the default dev build
# are untouched -- google/benchmark is a second external dependency and there is
# no reason to make every configure pay for it.

CPMAddPackage(
  NAME benchmark
  GITHUB_REPOSITORY google/benchmark
  VERSION 1.9.1
  OPTIONS
    "BENCHMARK_ENABLE_TESTING OFF"
    "BENCHMARK_ENABLE_INSTALL OFF"
    "BENCHMARK_ENABLE_GTEST_TESTS OFF"
    "BENCHMARK_INSTALL_DOCS OFF"
)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# add_hazptr_bench(target include_dir)
#   Same source, different header.  The include directory is the only
#   difference between the two binaries: both headers are named
#   hazard_ptr.hpp and both live in namespace proto, so they cannot coexist in
#   one translation unit and the comparison has to be made across binaries.
function(add_hazptr_bench name include_dir)
  add_executable(${name} bench_hazptr.cpp)
  target_include_directories(${name} PRIVATE ${include_dir})
  target_link_libraries(${name} PRIVATE benchmark::benchmark Threads::Threads)
  if(NOT MSVC)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_hazptr_bench(bench_current  ${CMAKE_SOURCE_DIR})
add_hazptr_bench(bench_baseline ${CMAKE_CURRENT_SOURCE_DIR}/baseline)
