// Tests for hazard_pointer_obj_base::retire() and HazardDomain::synchronize().
// Custom deleter tests live in test_custom_deleter.cpp.
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>

using proto::hazard_pointer_obj_base;
using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

namespace {
struct Tracked : hazard_pointer_obj_base<Tracked> {
  explicit Tracked(int& counter) : counter_(counter) {}
  ~Tracked() { ++counter_; }
  int& counter_;
};
} // namespace

TEST(Retire, RetiredObjectDeletedAfterCleanUp) {
  auto hp = make_hazard_pointer(); // keep active_count_=1 so threshold=2; retire(1) does not auto-sync
  int dtor_count = 0;
  (new Tracked(dtor_count))->retire();
  EXPECT_EQ(dtor_count, 0);
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 1);
}

TEST(Retire, MultipleObjectsAllDeleted) {
  int dtor_count = 0;
  for (int i = 0; i < 5; ++i)
    (new Tracked(dtor_count))->retire();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 5);
}

TEST(Retire, ProtectedObjectNotDeleted) {
  int dtor_count = 0;
  auto* obj = new Tracked(dtor_count);
  const std::atomic<Tracked*> src{obj};

  auto hp = make_hazard_pointer();
  const Tracked* const protected_obj = hp.protect(src);
  EXPECT_EQ(obj, protected_obj);

  obj->retire();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 0);

  hp.reset_protection();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 1);
}

TEST(Retire, RetireListGrowsBeforeCleanUp) {
  auto hp = make_hazard_pointer(); // keep active_count_=1 so threshold=2; retiring 2 does not auto-sync
  auto before = hazptr_default_domain().retire_list_size();
  int dtor_count = 0;
  (new Tracked(dtor_count))->retire();
  (new Tracked(dtor_count))->retire();
  EXPECT_EQ(hazptr_default_domain().retire_list_size(), before + 2);
  hazptr_default_domain().synchronize();
  EXPECT_EQ(hazptr_default_domain().retire_list_size(), 0u);
  EXPECT_EQ(dtor_count, 2);
}

TEST(Retire, ThresholdTriggersAutoCleanUp) {
  // 1 active slot -> threshold = 2; retiring 3 unprotected objects must trigger auto-synchronize.
  auto hp = make_hazard_pointer();
  int dummy_dtor = 0;
  Tracked x{dummy_dtor};
  const std::atomic<Tracked*> src{&x};
  const Tracked* const protected_x = hp.protect(src);
  EXPECT_EQ(&x, protected_x);

  int dtor_count = 0;
  for (int i = 0; i < 3; ++i)
    (new Tracked(dtor_count))->retire();

  EXPECT_GT(dtor_count, 0);
}
