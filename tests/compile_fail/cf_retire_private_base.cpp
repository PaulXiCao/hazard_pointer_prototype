// Compile-fail test: retire() must be rejected when hazard_pointer_obj_base is a
// private base of T, because is_convertible_v<T*, base*> fails for private inheritance.
// Expected error: static_assert in hazard_pointer_obj_base::retire().
#include <hazard_ptr.hpp>

struct Node : private proto::hazard_pointer_obj_base<Node> {
  using hazard_pointer_obj_base::retire; // expose retire() through private base
};

void test() {
  auto* p = new Node();
  p->retire(); // static_assert: is_convertible_v<Node*, hazard_pointer_obj_base<Node>*> fails
}
