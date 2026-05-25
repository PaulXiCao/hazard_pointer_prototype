// Tests for make_hazard_pointer().
#include <cstddef>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <vector>

using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

TEST(MakeHazardPointer, ReturnsNonEmptyHandle) {
  auto hp = make_hazard_pointer();
  EXPECT_FALSE(hp.empty());
}

TEST(MakeHazardPointer, IncrementsActiveSlots) {
  const auto before = hazptr_default_domain().active_slots();
  auto hp = make_hazard_pointer();
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 1);
}

TEST(MakeHazardPointer, MultipleCallsReturnDistinctSlots) {
  const auto before = hazptr_default_domain().active_slots();
  auto a = make_hazard_pointer();
  auto b = make_hazard_pointer();
  auto c = make_hazard_pointer();
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 3);
}

TEST(MakeHazardPointer, SlotsReusedAfterRelease) {
  const auto before = hazptr_default_domain().active_slots();
  {
    auto hp = make_hazard_pointer();
  }
  auto hp2 = make_hazard_pointer();
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 1);
}

TEST(MakeHazardPointer, PoolGrowsBeyondInitialCapacity) {
  const auto before = hazptr_default_domain().active_slots();
  constexpr std::size_t kExtra = 8 + 4; // kInitialSlots(8) + 4 to force pool growth
  std::vector<proto::hazard_pointer> hps;
  hps.reserve(kExtra);
  for (std::size_t i = 0; i < kExtra; ++i)
    hps.emplace_back(make_hazard_pointer());
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + kExtra);
}
