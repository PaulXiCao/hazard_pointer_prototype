// Compile-fail test: discarding the [[nodiscard]] return value of make_hazard_pointer()
// must produce a diagnostic escalated to an error by the CMake build rule.
#include <hazard_ptr.hpp>

void test() {
  proto::make_hazard_pointer(); // [[nodiscard]] result discarded; slot leaks
}
