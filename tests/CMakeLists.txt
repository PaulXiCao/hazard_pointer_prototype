set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# -- Regular GTest targets ----------------------------------------------------

function(add_hazptr_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE GTest::gtest_main Threads::Threads)

  if(MSVC)
    set(warn_opts /W4)
    if(WARNINGS_AS_ERRORS)
      list(APPEND warn_opts /WX)
    endif()
    target_compile_options(${name} PRIVATE ${warn_opts})
    if(SANITIZE_ADDRESS)
      target_compile_options(${name} PRIVATE /fsanitize=address)
    endif()
    if(SANITIZE_THREAD)
      message(WARNING "ThreadSanitizer not supported on MSVC -- skipping for ${name}")
    endif()
  else()
    set(warn_opts -Wall -Wextra -Wpedantic)
    if(WARNINGS_AS_ERRORS)
      list(APPEND warn_opts -Werror)
    endif()
    target_compile_options(${name} PRIVATE ${warn_opts})
    if(SANITIZE_THREAD)
      target_compile_options(${name} PRIVATE -fsanitize=thread -g)
      target_link_options(${name} PRIVATE -fsanitize=thread)
    endif()
    if(SANITIZE_ADDRESS)
      target_compile_options(${name} PRIVATE -fsanitize=address,undefined -g)
      target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()

  if(ENABLE_CLANG_TIDY)
    set_target_properties(${name} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
  endif()

  add_test(NAME ${name} COMMAND ${name})
endfunction()

# -- Compile-fail test helpers ------------------------------------------------
# These tests PASS when compilation fails (WILL_FAIL TRUE).
# Targets inherit the project C++ standard: CMAKE_CXX_STANDARD 26 (GCC/Clang)
# or /std:c++latest via the project-level add_compile_options (MSVC).

# add_compile_fail_test(name source)
#   Creates an OBJECT library from compile_fail/<source> that is expected to
#   fail to compile. CTest runs `cmake --build ... --target <name>` and marks
#   the test as PASSED when the build returns a non-zero exit code.
function(add_compile_fail_test name source)
  add_library(${name} OBJECT EXCLUDE_FROM_ALL compile_fail/${source})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target ${name} --config $<CONFIG>)
  set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
endfunction()

# add_nodiscard_fail_test(name source)
#   Like add_compile_fail_test but additionally promotes the [[nodiscard]]
#   warning to an error (-Werror=unused-result / /we4834) so that discarding
#   a [[nodiscard]] return value triggers a compilation failure.
function(add_nodiscard_fail_test name source)
  add_library(${name} OBJECT EXCLUDE_FROM_ALL compile_fail/${source})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/we4834>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror=unused-result>)
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target ${name} --config $<CONFIG>)
  set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
endfunction()

# -- Test registrations -------------------------------------------------------

# hazard_pointer_obj_base
add_hazptr_test(test_retire)
add_hazptr_test(test_custom_deleter)
# Replaces the global allocation functions, so it needs a binary of its own --
# and it cannot be built under ASan/TSan, whose runtimes define the same
# symbols (multiple definition of `operator delete(void*, unsigned long)').
# The allocation audit is orthogonal to what the sanitizers check, so skipping
# it there loses no coverage.
if(NOT SANITIZE_ADDRESS AND NOT SANITIZE_THREAD)
  add_hazptr_test(test_retire_no_alloc)
endif()

# hazard_pointer construction / destruction / move
add_hazptr_test(test_hazard_pointer_ctor)
add_hazptr_test(test_move_assign)

# hazard_pointer::empty()
add_hazptr_test(test_empty)

# make_hazard_pointer()
add_hazptr_test(test_make_hazard_pointer)

# hazard_pointer::protect() / try_protect() / reset_protection()
add_hazptr_test(test_protect)
add_hazptr_test(test_try_protect)
add_hazptr_test(test_reset_protection)

# hazard_pointer::swap() + free swap()
add_hazptr_test(test_swap)

# Compile-time properties (noexcept, type constraints)
add_hazptr_test(test_noexcept)
add_hazptr_test(test_type_constraints)

# Concurrency and thread-exit
add_hazptr_test(test_concurrent)
add_hazptr_test(test_thread_exit)

# -- Compile-fail tests: type constraints ------------------------------------
add_compile_fail_test(cf_protect_int              cf_protect_int.cpp)
add_compile_fail_test(cf_protect_nonhazard_class  cf_protect_nonhazard_class.cpp)
add_compile_fail_test(cf_retire_private_base      cf_retire_private_base.cpp)

# cf_retire_virtual_base requires C++26 std::is_virtual_base_of (P2985R0).
# Only register the test if the feature is available on the target compiler.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
  #ifndef __cpp_lib_is_virtual_base_of
  #  error is_virtual_base_of not available
  #endif
  int main() { return 0; }
" HAZPTR_HAS_IS_VIRTUAL_BASE_OF)

if(HAZPTR_HAS_IS_VIRTUAL_BASE_OF)
  add_compile_fail_test(cf_retire_virtual_base cf_retire_virtual_base.cpp)
endif()

# test_contracts uses EXPECT_DEATH on pre() violations -- only meaningful when
# compiled with -fcontracts. Skip registration entirely on other compilers so
# the test binary is not built and no empty-pass test appears in CTest output.
check_cxx_source_compiles("
  #ifndef __cpp_contracts
  #  error contracts not available
  #endif
  int main() { return 0; }
" HAZPTR_HAS_CONTRACTS)

if(HAZPTR_HAS_CONTRACTS)
  add_hazptr_test(test_contracts)
endif()

# -- Compile-fail tests: [[nodiscard]] ----------------------------------------
add_nodiscard_fail_test(cf_nodiscard_protect             cf_nodiscard_protect.cpp)
add_nodiscard_fail_test(cf_nodiscard_try_protect         cf_nodiscard_try_protect.cpp)
add_nodiscard_fail_test(cf_nodiscard_empty               cf_nodiscard_empty.cpp)
add_nodiscard_fail_test(cf_nodiscard_make_hazard_pointer cf_nodiscard_make_hazard_pointer.cpp)
