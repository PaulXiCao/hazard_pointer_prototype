// Tests for hazard_pointer::reset_protection() (both overloads).
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>

using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

namespace {
struct Node : proto::hazard_pointer_obj_base<Node> {};

struct Tracked : proto::hazard_pointer_obj_base<Tracked> {
  explicit Tracked(int& c) : counter(c) {}
  ~Tracked() { ++counter; }
  int& counter;
};
} // namespace

TEST(ResetProtection, SlotStillOwnedAfterReset) {
  // reset_protection clears the hazard but the handle keeps the slot.
  auto hp = make_hazard_pointer();
  Node x;
  const std::atomic<Node*> src{&x};
  (void)hp.protect(src);
  hp.reset_protection();
  EXPECT_FALSE(hp.empty());
}

TEST(ResetProtection, NullptrOverload_ClearsSlot) {
  auto hp = make_hazard_pointer();
  Node x;
  const std::atomic<Node*> src{&x};
  (void)hp.protect(src);
  hp.reset_protection(nullptr);
  EXPECT_FALSE(hp.empty());
}

TEST(ResetProtection, AfterReset_ObjectCanBeReclaimed) {
  auto hp = make_hazard_pointer();
  int dtor_count = 0;
  auto* obj = new Tracked(dtor_count);
  const std::atomic<Tracked*> src{obj};
  (void)hp.protect(src);

  obj->retire();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 0); // still protected

  hp.reset_protection();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 1);
}

TEST(ResetProtection, TypedPtrOverload_PublishesHazard) {
  // reset_protection(T*) stores the pointer directly -- used when the caller
  // already holds the pointer with correct ordering.
  auto hp = make_hazard_pointer();
  int dtor_b = 0;
  auto* obj_b = new Tracked(dtor_b);
  hp.reset_protection(obj_b); // protect obj_b without loading from an atomic

  obj_b->retire();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_b, 0); // protected via typed overload

  hp.reset_protection();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_b, 1);
}

TEST(ResetProtection, DoubleReset_IsSafe) {
  auto hp = make_hazard_pointer();
  Node x;
  const std::atomic<Node*> src{&x};
  (void)hp.protect(src);
  hp.reset_protection();
  hp.reset_protection(); // idempotent
  EXPECT_FALSE(hp.empty());
}
