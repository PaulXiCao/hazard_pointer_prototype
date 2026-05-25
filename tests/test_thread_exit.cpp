// Tests for thread-exit behavior: retire list draining and orphan list.
// The thread-local ~RetireListNode() synchronizes on exit and offloads survivors
// to the domain's orphan list for eventual reclamation.
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <thread>

using proto::hazard_pointer_obj_base;
using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

namespace {
struct Tracked : hazard_pointer_obj_base<Tracked> {
  explicit Tracked(int& c) : counter(c) {}
  ~Tracked() { ++counter; }
  int& counter;
};
} // namespace

TEST(ThreadExit, RetiredObjects_CleanedOnThreadExit) {
  // A thread retires unprotected objects and exits.
  // ~RetireListNode() synchronizes and reclaims all of them.
  int dtor_count = 0;
  std::thread t([&] {
    for (int i = 0; i < 3; ++i)
      (new Tracked(dtor_count))->retire();
  });
  t.join();
  EXPECT_EQ(dtor_count, 3);
}

TEST(ThreadExit, ProtectedSurvivors_LandInOrphanList_ThenReclaimed) {
  // Objects that are still protected when a thread exits go to the orphan list.
  // A subsequent synchronize() from any thread reclaims them once protection drops.
  int dtor_count = 0;
  auto* obj = new Tracked(dtor_count);
  const std::atomic<Tracked*> src{obj};

  auto hp = make_hazard_pointer(); // main-thread HP protects obj
  (void)hp.protect(src);

  std::thread t([&] {
    obj->retire(); // retired from worker thread, protected by main thread's HP
                   // On exit: ~RetireListNode() synchronizes -- obj survives -> goes to orphan_list_.
  });
  t.join();

  EXPECT_EQ(dtor_count, 0); // still protected by hp

  hp.reset_protection();
  hazptr_default_domain().synchronize(); // drains orphan_list_
  EXPECT_EQ(dtor_count, 1);
}

TEST(ThreadExit, MultipleThreads_EachDrainOwnList) {
  // N threads each retire their own unprotected objects.
  // All are reclaimed on thread exit without any explicit synchronize() call.
  constexpr int kThreads = 4;
  constexpr int kObjectsPerThread = 5;
  int dtor_counts[kThreads] = {};

  std::thread threads[kThreads];
  for (int i = 0; i < kThreads; ++i) {
    threads[i] = std::thread([&dtor_counts, i] {
      for (int j = 0; j < kObjectsPerThread; ++j)
        (new Tracked(dtor_counts[i]))->retire();
    });
  }
  for (auto& t : threads)
    t.join();

  for (int i = 0; i < kThreads; ++i)
    EXPECT_EQ(dtor_counts[i], kObjectsPerThread) << "thread " << i;
}
