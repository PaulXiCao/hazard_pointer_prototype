// Tests for hazard_pointer constructors and destructor.
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <utility>

using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

TEST(HazardPointerCtor, DefaultConstructed_IsEmpty) {
  const proto::hazard_pointer hp;
  EXPECT_TRUE(hp.empty());
}

TEST(HazardPointerCtor, DefaultConstructed_DoesNotIncrementSlotCount) {
  const auto before = hazptr_default_domain().active_slots();
  const proto::hazard_pointer hp;
  EXPECT_EQ(hazptr_default_domain().active_slots(), before);
}

TEST(HazardPointerCtor, MoveCtor_TransfersOwnership) {
  const auto before = hazptr_default_domain().active_slots();
  auto a = make_hazard_pointer();
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 1);
  const proto::hazard_pointer b = std::move(a);
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 1);
}

TEST(HazardPointerCtor, MoveCtor_SourceBecomesEmpty) {
  auto a = make_hazard_pointer();
  const proto::hazard_pointer b = std::move(a);
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(b.empty());
}

TEST(HazardPointerCtor, Destructor_ReleasesSlot) {
  const auto before = hazptr_default_domain().active_slots();
  {
    auto hp = make_hazard_pointer();
    EXPECT_EQ(hazptr_default_domain().active_slots(), before + 1);
  }
  EXPECT_EQ(hazptr_default_domain().active_slots(), before);
}

TEST(HazardPointerCtor, Destructor_DefaultConstructed_IsNoOp) {
  const auto before = hazptr_default_domain().active_slots();
  {
    const proto::hazard_pointer hp;
  }
  EXPECT_EQ(hazptr_default_domain().active_slots(), before);
}

TEST(HazardPointerCtor, MultipleHazardPointers_AreDistinct) {
  const auto before = hazptr_default_domain().active_slots();
  auto a = make_hazard_pointer();
  auto b = make_hazard_pointer();
  auto c = make_hazard_pointer();
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 3);
}
