// Compile-fail test: discarding the [[nodiscard]] return value of protect()
// must produce a diagnostic escalated to an error by the CMake build rule.
// The add_nodiscard_fail_test helper adds -Werror=unused-result / /we4834.
#include <atomic>
#include <hazard_ptr.hpp>

namespace {
struct Node : proto::hazard_pointer_obj_base<Node> {};
} // namespace

void test() {
  auto hp = proto::make_hazard_pointer();
  Node x;
  std::atomic<Node*> src{&x};
  hp.protect(src); // [[nodiscard]] result discarded
}
