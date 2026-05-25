// Compile-fail test: protect<int>() must be rejected because int is not a class type
// and therefore does not satisfy HazardProtectable<int>.
// Expected error: static_assert in hazard_pointer::protect<int>().
#include <atomic>
#include <hazard_ptr.hpp>

void test() {
  auto hp = proto::make_hazard_pointer();
  std::atomic<int*> src{nullptr};
  (void)hp.protect(src); // int is not HazardProtectable -> static_assert fires
}
