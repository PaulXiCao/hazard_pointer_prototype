// Compile-fail test: protect<Plain>() must be rejected because Plain does not
// derive from hazard_pointer_obj_base and therefore fails HazardProtectable<Plain>.
// Expected error: static_assert in hazard_pointer::protect<Plain>().
#include <atomic>
#include <hazard_ptr.hpp>

struct Plain {};

void test() {
  auto hp = proto::make_hazard_pointer();
  std::atomic<Plain*> src{nullptr};
  (void)hp.protect(src); // Plain has no hazard_obj_base_tag base -> concept unsatisfied
}
