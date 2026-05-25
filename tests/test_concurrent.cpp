// Multi-threaded correctness tests: readers protect, writers retire.
// Run under TSan: cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
#include <atomic>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <thread>
#include <vector>

using proto::hazard_pointer_obj_base;
using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

namespace {
struct Node : hazard_pointer_obj_base<Node> {
  int value;
  explicit Node(int v) : value(v) {}
};

void reader_fn(std::atomic<Node*>& shared, std::atomic<bool>& stop, std::atomic<int>& errors) {
  auto hp = make_hazard_pointer();
  while (!stop.load(std::memory_order_acquire)) {
    const Node* const p = hp.protect(shared);
    if (p && p->value < 0)
      errors.fetch_add(1, std::memory_order_relaxed);
    hp.reset_protection();
  }
}

void writer_fn(std::atomic<Node*>& shared, std::atomic<bool>& stop) {
  int counter = 0;
  while (!stop.load(std::memory_order_acquire)) {
    Node* next = new Node(++counter);
    Node* const old = shared.exchange(next, std::memory_order_seq_cst);
    if (old)
      old->retire();
  }
  Node* final = shared.exchange(nullptr, std::memory_order_seq_cst);
  if (final)
    final->retire();
}
} // namespace

TEST(Concurrent, ReadersAndWriterNoDataRace) {
  std::atomic<Node*> shared{new Node(1)};
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};

  constexpr int kReaders = 4;
  std::vector<std::thread> threads;
  threads.reserve(kReaders + 1);

  for (int i = 0; i < kReaders; ++i)
    threads.emplace_back(reader_fn, std::ref(shared), std::ref(stop), std::ref(errors));
  threads.emplace_back(writer_fn, std::ref(shared), std::ref(stop));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true, std::memory_order_release);
  for (auto& t : threads)
    t.join();

  hazptr_default_domain().synchronize();
  EXPECT_EQ(errors.load(), 0);
}

TEST(Concurrent, ProtectSeesLatestValue) {
  auto hp = make_hazard_pointer();
  Node a{10}, b{20};
  std::atomic<Node*> src{&a};

  const Node* const p1 = hp.protect(src);
  EXPECT_EQ(p1, &a);

  src.store(&b, std::memory_order_release);
  const Node* const p2 = hp.protect(src);
  EXPECT_EQ(p2, &b);
}

TEST(Concurrent, MultipleWriters_MultipleReaders_NoDataRace) {
  // N readers protect, M writers each have their own atomic and retire old nodes.
  constexpr int kReaders = 4;
  constexpr int kWriters = 4;

  std::vector<std::atomic<Node*>> srcs(kWriters);
  for (auto& s : srcs)
    s.store(new Node(1), std::memory_order_relaxed);

  std::atomic_bool stop{false};
  std::atomic_int errors{0};

  auto multi_reader = [&] {
    std::vector<proto::hazard_pointer> hps;
    hps.reserve(kWriters);
    for (int i = 0; i < kWriters; ++i)
      hps.emplace_back(make_hazard_pointer());
    while (!stop.load(std::memory_order_acquire)) {
      for (int i = 0; i < kWriters; ++i) {
        const Node* const p = hps[i].protect(srcs[i]);
        if (p && p->value < 0)
          errors.fetch_add(1, std::memory_order_relaxed);
        hps[i].reset_protection();
      }
    }
  };

  auto single_writer = [&](int idx) {
    int counter = 1;
    while (!stop.load(std::memory_order_acquire)) {
      Node* next = new Node(++counter);
      Node* old = srcs[idx].exchange(next, std::memory_order_seq_cst);
      if (old)
        old->retire();
    }
    Node* final = srcs[idx].exchange(nullptr, std::memory_order_seq_cst);
    if (final)
      final->retire();
  };

  std::vector<std::thread> threads;
  threads.reserve(kReaders + kWriters);
  for (int i = 0; i < kReaders; ++i)
    threads.emplace_back(multi_reader);
  for (int i = 0; i < kWriters; ++i)
    threads.emplace_back(single_writer, i);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true, std::memory_order_release);
  for (auto& t : threads)
    t.join();

  hazptr_default_domain().synchronize();
  EXPECT_EQ(errors.load(), 0);
}

TEST(Concurrent, RetireFromMultipleThreads_AllReclaimed) {
  // Each thread retires its own objects; after all threads exit, everything is deleted.
  constexpr int kThreads = 8;
  constexpr int kObjectsPerThread = 10;
  std::atomic<int> total_deleted{0};

  struct CountedNode : hazard_pointer_obj_base<CountedNode> {
    std::atomic<int>& counter;
    explicit CountedNode(std::atomic<int>& c) : counter(c) {}
    ~CountedNode() { counter.fetch_add(1, std::memory_order_relaxed); }
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < kObjectsPerThread; ++j)
        (new CountedNode(total_deleted))->retire();
      // Thread exit drains the retire list.
    });
  }
  for (auto& t : threads)
    t.join();

  EXPECT_EQ(total_deleted.load(), kThreads * kObjectsPerThread);
}

TEST(Concurrent, HighContention_ProtectAndRetire_NoUseAfterFree) {
  // Many threads simultaneously protect-read-reset while a writer retires old nodes.
  // TSan should detect any use-after-free if protection ever fails.
  constexpr int kReaders = 6;
  std::atomic<Node*> shared{new Node(42)};
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};

  std::vector<std::thread> threads;
  threads.reserve(kReaders + 1);

  for (int i = 0; i < kReaders; ++i)
    threads.emplace_back(reader_fn, std::ref(shared), std::ref(stop), std::ref(errors));
  threads.emplace_back(writer_fn, std::ref(shared), std::ref(stop));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_release);
  for (auto& t : threads)
    t.join();

  hazptr_default_domain().synchronize();
  EXPECT_EQ(errors.load(), 0);
}
