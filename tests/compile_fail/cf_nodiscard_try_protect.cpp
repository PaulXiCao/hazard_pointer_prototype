// Compile-fail test: discarding the [[nodiscard]] return value of try_protect()
// must produce a diagnostic escalated to an error by the CMake build rule.
#include <atomic>
#include <hazard_ptr.hpp>

namespace {
struct Node : proto::hazard_pointer_obj_base<Node> {};
} // namespace

void test() {
  auto hp = proto::make_hazard_pointer();
  Node x;
  std::atomic<Node*> src{&x};
  Node* ptr = &x;
  hp.try_protect(ptr, src); // [[nodiscard]] result discarded
}
