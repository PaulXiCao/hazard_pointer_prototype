// Tests for hazard_pointer::swap() and the free swap() function.
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>

using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

namespace {
struct Tracked : proto::hazard_pointer_obj_base<Tracked> {
  explicit Tracked(int& c) : counter(c) {}
  ~Tracked() { ++counter; }
  int& counter;
};
} // namespace

TEST(Swap, MemberSwap_ExchangesNonEmptyAndEmpty) {
  auto a = make_hazard_pointer();
  proto::hazard_pointer b;
  EXPECT_FALSE(a.empty());
  EXPECT_TRUE(b.empty());
  a.swap(b);
  EXPECT_TRUE(a.empty());
  EXPECT_FALSE(b.empty());
}

TEST(Swap, MemberSwap_BothNonEmpty_SlotCountUnchanged) {
  const auto before = hazptr_default_domain().active_slots();
  auto a = make_hazard_pointer();
  auto b = make_hazard_pointer();
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 2);
  a.swap(b);
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 2);
}

TEST(Swap, MemberSwap_ProtectionFollowsSlot) {
  // After swap, the slot that held the hazard moves to the other handle.
  auto hp1 = make_hazard_pointer();
  auto hp2 = make_hazard_pointer();

  int dtor = 0;
  auto* obj = new Tracked(dtor);
  const std::atomic<Tracked*> src{obj};
  (void)hp1.protect(src);

  // hp2 now holds the protecting slot; hp1 has hp2's old (unassociated) slot.
  hp1.swap(hp2);

  obj->retire();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor, 0); // hp2 still protects obj

  hp2.reset_protection();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor, 1);
}

TEST(Swap, FreeSwap_ExchangesSlots) {
  auto a = make_hazard_pointer();
  proto::hazard_pointer b;
  EXPECT_FALSE(a.empty());
  EXPECT_TRUE(b.empty());
  proto::swap(a, b);
  EXPECT_TRUE(a.empty());
  EXPECT_FALSE(b.empty());
}

TEST(Swap, FreeSwap_BothNonEmpty_SlotCountUnchanged) {
  const auto before = hazptr_default_domain().active_slots();
  auto a = make_hazard_pointer();
  auto b = make_hazard_pointer();
  proto::swap(a, b);
  EXPECT_EQ(hazptr_default_domain().active_slots(), before + 2);
}

TEST(Swap, MemberSwap_SelfSwap_IsNoOp) {
  auto hp = make_hazard_pointer();
  EXPECT_FALSE(hp.empty());
  hp.swap(hp);
  EXPECT_FALSE(hp.empty());
}

TEST(Swap, FreeSwap_BothEmpty_RemainsEmpty) {
  proto::hazard_pointer a;
  proto::hazard_pointer b;
  proto::swap(a, b);
  EXPECT_TRUE(a.empty());
  EXPECT_TRUE(b.empty());
}
