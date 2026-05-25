// Compile-fail test: retire() must be rejected when hazard_pointer_obj_base is a
// virtual base of T. Requires C++26 std::is_virtual_base_of (P2985R0,
// __cpp_lib_is_virtual_base_of). If not available, this file compiles without error
// and the CMake function add_compile_fail_test_if_available skips the test.
// Expected error: static_assert in hazard_pointer_obj_base::retire().
#include <hazard_ptr.hpp>

struct Node : virtual proto::hazard_pointer_obj_base<Node> {};

void test() {
  auto* p = new Node();
  p->retire(); // static_assert: !is_virtual_base_of_v<hazard_pointer_obj_base<Node>, Node> fires
}
