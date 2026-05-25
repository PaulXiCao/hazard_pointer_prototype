// Tests for hazard_pointer move assignment operator.
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <utility>

using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

TEST(MoveAssign, TransfersSlotOwnership) {
  const auto before = hazptr_default_domain().active_slots();
  auto a = make_hazard_pointer();
  proto::hazard_pointer b;
  EXPECT_FALSE(a.empty());
  EXPECT_TRUE(b.empty());
  b = std::move(a);
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(b.empty());
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 1);
}

TEST(MoveAssign, SourceBecomesEmpty) {
  auto a = make_hazard_pointer();
  proto::hazard_pointer b;
  b = std::move(a);
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(MoveAssign, DestReleasesPriorSlot) {
  const auto before = hazptr_default_domain().active_slots();
  auto a = make_hazard_pointer();
  auto b = make_hazard_pointer();
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 2);
  b = std::move(a);
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 1);
}

TEST(MoveAssign, SelfAssign_IsNoOp) {
  auto hp = make_hazard_pointer();
  const auto before = hazptr_default_domain().active_slots();
  // Test that the explicit `if (this == &other) return *this` guard works.
  // Use a reference to avoid -Wself-move; the guard fires at runtime.
  auto& alias = hp;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#endif
  hp = std::move(alias); // NOLINT(bugprone-use-after-move)
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  EXPECT_EQ(hazptr_default_domain().active_slots(), before);
}

TEST(MoveAssign, AssignToEmpty_BothEmpty_NoSlotChange) {
  const auto before = hazptr_default_domain().active_slots();
  proto::hazard_pointer a;
  proto::hazard_pointer b;
  b = std::move(a); // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(hazptr_default_domain().active_slots(), before);
}
