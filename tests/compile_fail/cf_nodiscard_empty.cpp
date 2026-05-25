// Compile-fail test: discarding the [[nodiscard]] return value of empty()
// must produce a diagnostic escalated to an error by the CMake build rule.
#include <hazard_ptr.hpp>

void test() {
  auto hp = proto::make_hazard_pointer();
  hp.empty(); // [[nodiscard]] result discarded
}
