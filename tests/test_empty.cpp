// Tests for hazard_pointer::empty().
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <utility>

using proto::make_hazard_pointer;

namespace {
struct Node : proto::hazard_pointer_obj_base<Node> {};
} // namespace

TEST(Empty, DefaultConstructed_IsEmpty) {
  const proto::hazard_pointer hp;
  EXPECT_TRUE(hp.empty());
}

TEST(Empty, AfterAcquire_NotEmpty) {
  auto hp = make_hazard_pointer();
  EXPECT_FALSE(hp.empty());
}

TEST(Empty, AfterMoveCtor_SourceIsEmpty) {
  auto a = make_hazard_pointer();
  const proto::hazard_pointer b = std::move(a);
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(b.empty());
}

TEST(Empty, AfterMoveAssign_SourceIsEmpty) {
  auto a = make_hazard_pointer();
  proto::hazard_pointer b;
  b = std::move(a);
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(b.empty());
}

TEST(Empty, AfterProtect_StillNotEmpty) {
  auto hp = make_hazard_pointer();
  Node x;
  const std::atomic<Node*> src{&x};
  (void)hp.protect(src);
  EXPECT_FALSE(hp.empty());
}

TEST(Empty, AfterResetProtection_StillNotEmpty) {
  // reset_protection clears the hazard but the handle still owns the slot.
  auto hp = make_hazard_pointer();
  Node x;
  const std::atomic<Node*> src{&x};
  (void)hp.protect(src);
  hp.reset_protection();
  EXPECT_FALSE(hp.empty());
}

TEST(Empty, AfterSwap_OwnershipExchanged) {
  auto a = make_hazard_pointer();
  proto::hazard_pointer b;
  EXPECT_FALSE(a.empty());
  EXPECT_TRUE(b.empty());
  a.swap(b);
  EXPECT_TRUE(a.empty());
  EXPECT_FALSE(b.empty());
}
