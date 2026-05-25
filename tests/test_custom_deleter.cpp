// Tests for hazard_pointer_obj_base::retire() with custom deleter types.
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>

using proto::hazard_pointer_obj_base;
using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

namespace {

// LoggingDeleter calls delete and increments a counter.
// Forward-declare LoggingNode so the operator() signature can name it.
struct LoggingNode;

struct LoggingDeleter {
  int* delete_count = nullptr;
  LoggingDeleter() = default;
  explicit LoggingDeleter(int* c) : delete_count(c) {}
  void operator()(LoggingNode* p) const;
};

struct LoggingNode : hazard_pointer_obj_base<LoggingNode, LoggingDeleter> {};

inline void LoggingDeleter::operator()(LoggingNode* p) const {
  if (delete_count)
    ++(*delete_count);
  delete p;
}

// Simple tracked node using the default deleter.
struct TrackedNode : hazard_pointer_obj_base<TrackedNode> {
  explicit TrackedNode(int& c) : counter(c) {}
  ~TrackedNode() { ++counter; }
  int& counter;
};

} // namespace

TEST(CustomDeleter, DefaultDeleter_CallsDelete) {
  int dtor_count = 0;
  auto* node = new TrackedNode(dtor_count);
  node->retire();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 1);
}

TEST(CustomDeleter, FunctorDeleter_IsInvoked) {
  int delete_count = 0;
  auto* node = new LoggingNode();
  node->retire(LoggingDeleter{&delete_count});
  hazptr_default_domain().synchronize();
  EXPECT_EQ(delete_count, 1);
}

TEST(CustomDeleter, FunctorDeleter_NotCalledWhileProtected) {
  int delete_count = 0;
  auto* node = new LoggingNode();
  const std::atomic<LoggingNode*> src{node};

  auto hp = make_hazard_pointer();
  (void)hp.protect(src);
  node->retire(LoggingDeleter{&delete_count});
  hazptr_default_domain().synchronize();
  EXPECT_EQ(delete_count, 0); // still protected

  hp.reset_protection();
  hazptr_default_domain().synchronize();
  EXPECT_EQ(delete_count, 1);
}

TEST(CustomDeleter, DefaultConstructedDeleter_UsedWhenNoArgPassed) {
  // retire() uses a default-constructed D when called with no argument.
  int dtor_count = 0;
  auto* node = new TrackedNode(dtor_count);
  node->retire(); // D = std::default_delete<TrackedNode>, default-constructed
  hazptr_default_domain().synchronize();
  EXPECT_EQ(dtor_count, 1);
}

TEST(CustomDeleter, MultipleNodesWithCustomDeleter_AllInvoked) {
  constexpr int kCount = 5;
  int delete_count = 0;
  for (int i = 0; i < kCount; ++i) {
    auto* node = new LoggingNode();
    node->retire(LoggingDeleter{&delete_count});
  }
  hazptr_default_domain().synchronize();
  EXPECT_EQ(delete_count, kCount);
}
