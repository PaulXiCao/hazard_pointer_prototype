// Tests for hazard_pointer::protect().
// reset_protection() tests live in test_reset_protection.cpp.
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>

using proto::make_hazard_pointer;

namespace {
struct Node : proto::hazard_pointer_obj_base<Node> {
  int value = 0;
};
} // namespace

TEST(Protect, ReturnsCurrentValue) {
  auto hp = make_hazard_pointer();
  Node x;
  x.value = 42;
  const std::atomic<Node*> src{&x};
  const Node* const p = hp.protect(src);
  ASSERT_EQ(p, &x);
  EXPECT_EQ(p->value, 42);
}

TEST(Protect, ReturnsNullForNullAtomic) {
  auto hp = make_hazard_pointer();
  const std::atomic<Node*> src{nullptr};
  EXPECT_EQ(hp.protect(src), nullptr);
}

TEST(Protect, SlotNotEmptyAfterProtect) {
  auto hp = make_hazard_pointer();
  Node x;
  const std::atomic<Node*> src{&x};
  (void)hp.protect(src);
  EXPECT_FALSE(hp.empty());
}

TEST(Protect, ProtectMatchesAtomicLoad) {
  auto hp = make_hazard_pointer();
  Node vals[3];
  vals[0].value = 1;
  vals[1].value = 2;
  vals[2].value = 3;
  const std::atomic<Node*> src{&vals[0]};
  const Node* const p = hp.protect(src);
  EXPECT_EQ(p, src.load());
}

TEST(Protect, ReprotectUpdatesSlot) {
  auto hp = make_hazard_pointer();
  Node a, b;
  std::atomic<Node*> src{&a};
  (void)hp.protect(src);
  src.store(&b, std::memory_order_relaxed);
  const Node* const p = hp.protect(src);
  EXPECT_EQ(p, &b);
}