// Tests for hazard_pointer::try_protect().
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>

using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

namespace {
struct Node : proto::hazard_pointer_obj_base<Node> {
  int value = 0;
};

struct Tracked : proto::hazard_pointer_obj_base<Tracked> {
  explicit Tracked(int& c) : counter(c) {}
  ~Tracked() { ++counter; }
  int& counter;
};
} // namespace

TEST(TryProtect, ReturnsTrueForStablePointer) {
  auto hp = make_hazard_pointer();
  Node x;
  const std::atomic<Node*> src{&x};
  Node* ptr = src.load(std::memory_order::relaxed); // NOLINT(misc-const-correctness)
  EXPECT_TRUE(hp.try_protect(ptr, src));
}

TEST(TryProtect, SetsPtrOnSuccess) {
  auto hp = make_hazard_pointer();
  Node x;
  x.value = 7;
  const std::atomic<Node*> src{&x};
  Node* ptr = src.load(std::memory_order::relaxed); // NOLINT(misc-const-correctness)
  const bool ok = hp.try_protect(ptr, src);
  EXPECT_TRUE(ok);
  EXPECT_EQ(ptr, &x);
}

TEST(TryProtect, ReturnsTrueForNullSrc) {
  auto hp = make_hazard_pointer();
  const std::atomic<Node*> src{nullptr};
  Node* ptr = nullptr; // NOLINT(misc-const-correctness)
  EXPECT_TRUE(hp.try_protect(ptr, src));
  EXPECT_EQ(ptr, nullptr);
}

TEST(TryProtect, SlotStillOwnedAfterSuccess) {
  auto hp = make_hazard_pointer();
  Node x;
  const std::atomic<Node*> src{&x};
  Node* ptr = &x; // NOLINT(misc-const-correctness)
  (void)hp.try_protect(ptr, src);
  EXPECT_FALSE(hp.empty());
}

TEST(TryProtect, ProtectedObject_NotReclaimedAfterSuccess) {
  auto hp = make_hazard_pointer();
  int dtor_count = 0;
  auto* obj = new Tracked(dtor_count);
  const std::atomic<Tracked*> src{obj};
  Tracked* ptr = src.load(std::memory_order::relaxed); // NOLINT(misc-const-correctness)
  ASSERT_TRUE(hp.try_protect(ptr, src));

  obj->retire();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 0); // still protected

  hp.reset_protection();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 1);
}

TEST(TryProtect, MatchesProtect_ForStablePointer) {
  // try_protect and protect should agree when the pointer doesn't change.
  Node x;
  const std::atomic<Node*> src{&x};

  auto hp1 = make_hazard_pointer();
  auto hp2 = make_hazard_pointer();

  Node* ptr = src.load(std::memory_order::relaxed); // NOLINT(misc-const-correctness)
  const bool ok = hp1.try_protect(ptr, src);
  const Node* const via_protect = hp2.protect(src);

  EXPECT_TRUE(ok);
  EXPECT_EQ(ptr, via_protect);
  EXPECT_EQ(ptr, &x);
}
