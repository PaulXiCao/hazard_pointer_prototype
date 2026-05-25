// Contract precondition death tests. Only meaningful when compiled with
// -fcontracts (gcc16-contracts CI job). Each test verifies that calling a
// function with a violated precondition terminates the process.
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>

#ifdef __cpp_contracts

using proto::make_hazard_pointer;

namespace {
struct Node : proto::hazard_pointer_obj_base<Node> {};
} // namespace

TEST(ContractDeathTest, TryProtectOnEmpty) {
  proto::hazard_pointer hp;
  Node* ptr = nullptr;
  const std::atomic<Node*> src{nullptr};
  EXPECT_DEATH((void)hp.try_protect(ptr, src), "");
}

TEST(ContractDeathTest, ResetProtectionNullOnEmpty) {
  proto::hazard_pointer hp;
  EXPECT_DEATH(hp.reset_protection(), "");
}

TEST(ContractDeathTest, ResetProtectionPtrOnEmpty) {
  proto::hazard_pointer hp;
  Node x;
  EXPECT_DEATH(hp.reset_protection(&x), "");
}

TEST(ContractDeathTest, DoubleRetire) {
  // hp protects p so synchronize() cannot reclaim it between the two retire()
  // calls; the second retire() reaches pre(!retired_) while p is still alive.
  EXPECT_DEATH(
      {
        auto hp = make_hazard_pointer();
        auto* p = new Node;
        const std::atomic<Node*> src{p};
        (void)hp.protect(src);
        p->retire();
        p->retire(); // pre(!retired_) fires
      },
      "");
}

#endif // __cpp_contracts
