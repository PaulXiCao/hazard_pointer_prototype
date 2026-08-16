// Multi-threaded correctness tests: readers protect, writers retire.
// Run under TSan: cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
// Run under ASan: cmake --preset asan && cmake --build --preset asan && ctest --preset asan
#include <atomic>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <mutex>
#include <thread>
#include <vector>

using proto::hazard_pointer_obj_base;
using proto::make_hazard_pointer;
using proto::detail::hazptr_default_domain;

namespace {

// --- Quarantined nodes: making reclamation observable ------------------------
//
// Review point 4 (Thomas Rodgers): the reader check here used to be
// `p->value < 0` while writers only ever published `++counter`, so it could
// never fire -- these tests could not detect the use-after-free at all.
//
// The obvious repair, having ~Node() write a negative sentinel, does not work
// either.  `delete p` runs the destructor and then operator delete, and glibc
// immediately writes its tcache next/key over the first 16 bytes of the chunk.
// Node::value sits at offset 4 (sizeof(Node) == 8), so the sentinel is replaced
// by allocator bookkeeping -- which reads back as a small *positive* int --
// nanoseconds after it is written.  The writer's next allocation then hands the
// same chunk straight back, tcache being LIFO.  Measured on x86_64/glibc: the
// sentinel was not visible once.
//
// So reclaimed nodes must not go back to the allocator.  The deleter poisons
// the node and parks it here; the quarantine is drained only at process exit.
// The memory stays valid, so a reader that dereferences a node the domain has
// already reclaimed reliably observes kPoisoned -- on every target, with no
// dependence on a sanitizer, and without reading freed memory (which would be
// UB the compiler is entitled to exploit).
constexpr int kPoisoned = -1;

// Quarantined nodes are never freed during a run, so the writers below are
// bounded by a node budget as well as by the stop flag.  At 8 bytes per node
// plus a pointer here, this is roughly 1.6 MB across the four-writer test.
constexpr int kMaxNodesPerWriter = 50000;

struct QuarantinedNode;

struct Quarantine {
  std::vector<QuarantinedNode*> nodes;
  std::mutex nodes_mtx;

  void reclaim(QuarantinedNode* ptr);
  ~Quarantine();
};

// constinit is load-bearing, not decoration.  ~HazardDomain() drains the orphan
// list through this deleter during static-storage destruction, so the
// quarantine has to still be alive at that point.  Constant initialization puts
// its construction before any dynamic initialization, and therefore its
// destruction after the function-local static domain in
// hazptr_default_domain().  Spelling it constinit makes the compiler enforce
// that: a future member without a constexpr default ctor then breaks the build
// here rather than the test at exit.
constinit Quarantine quarantine;

// A separate stateless type is required: retire() needs D to be default
// constructible and move assignable, which Quarantine's mutex rules out.
struct QuarantineDeleter {
  void operator()(QuarantinedNode* ptr) const { quarantine.reclaim(ptr); }
};

struct QuarantinedNode : hazard_pointer_obj_base<QuarantinedNode, QuarantineDeleter> {
  int value;
  explicit QuarantinedNode(int v) : value(v) {}
};

// Taking nodes_mtx cannot deadlock against the domain: both deleter call sites
// run with no domain lock held -- see synchronize() step 3 ("No lock held here")
// and ~HazardDomain().
//
// `value` is a plain int on purpose.  If the reclaim-side ordering is ever
// wrong, this store races the reader's load of the same int and TSan reports
// it, so one bug has two independent detectors.  A correct implementation has
// no race here and TSan stays clean.
void Quarantine::reclaim(QuarantinedNode* ptr) {
  ptr->value = kPoisoned;
  const std::lock_guard _{nodes_mtx};
  nodes.push_back(ptr);
}

Quarantine::~Quarantine() {
  for (QuarantinedNode* ptr : nodes)
    delete ptr;
}

// reads counts successful protects, so a test cannot pass by never observing a
// node at all -- that vacuity is exactly what review point 4 was about.
void quarantined_reader_fn(std::atomic<QuarantinedNode*>& shared, std::atomic<bool>& stop, std::atomic<int>& errors,
                           std::atomic<int>& reads) {
  auto hp = make_hazard_pointer();
  while (!stop.load(std::memory_order_acquire)) {
    const QuarantinedNode* const p = hp.protect(shared);
    if (p) {
      reads.fetch_add(1, std::memory_order_relaxed);
      if (p->value == kPoisoned)
        errors.fetch_add(1, std::memory_order_relaxed);
    }
    hp.reset_protection();
  }
}

void quarantined_writer_fn(std::atomic<QuarantinedNode*>& shared, std::atomic<bool>& stop) {
  int counter = 0;
  while (!stop.load(std::memory_order_acquire) && counter < kMaxNodesPerWriter) {
    QuarantinedNode* next = new QuarantinedNode(++counter);
    QuarantinedNode* const old = shared.exchange(next, std::memory_order_seq_cst);
    if (old)
      old->retire();
  }
  QuarantinedNode* final = shared.exchange(nullptr, std::memory_order_seq_cst);
  if (final)
    final->retire();
}

// --- Plain nodes: the real operator delete path ------------------------------
//
// Kept on the default deleter so that one test still frees for real while
// readers are running.  Its detector is ASan, not a value check: per the note
// above, no sentinel can survive the free, so the dereference itself is the
// point -- a failed protection surfaces as heap-use-after-free.
struct Node : hazard_pointer_obj_base<Node> {
  int value;
  explicit Node(int v) : value(v) {}
};

// observed is a store, not an accumulator: summing values would overflow int
// over millions of iterations, and UBSan is enabled in the asan preset.  It
// starts at kNothingObserved, which writers never publish, so the test can
// still tell "read nothing" from "read something".
constexpr int kNothingObserved = -1;

void reader_fn(std::atomic<Node*>& shared, std::atomic<bool>& stop, std::atomic<int>& observed) {
  auto hp = make_hazard_pointer();
  while (!stop.load(std::memory_order_acquire)) {
    const Node* const p = hp.protect(shared);
    if (p)
      observed.store(p->value, std::memory_order_relaxed);
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
  std::atomic<QuarantinedNode*> shared{new QuarantinedNode(1)};
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::atomic<int> reads{0};

  constexpr int kReaders = 4;
  std::vector<std::thread> threads;
  threads.reserve(kReaders + 1);

  for (int i = 0; i < kReaders; ++i)
    threads.emplace_back(quarantined_reader_fn, std::ref(shared), std::ref(stop), std::ref(errors), std::ref(reads));
  threads.emplace_back(quarantined_writer_fn, std::ref(shared), std::ref(stop));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true, std::memory_order_release);
  for (auto& t : threads)
    t.join();

  hazptr_default_domain().synchronize();
  EXPECT_EQ(errors.load(), 0) << "reader dereferenced a node the domain had already reclaimed";
  EXPECT_GT(reads.load(), 0) << "no reader ever observed a node; the test proved nothing";
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

  std::vector<std::atomic<QuarantinedNode*>> srcs(kWriters);
  for (auto& s : srcs)
    s.store(new QuarantinedNode(1), std::memory_order_relaxed);

  std::atomic_bool stop{false};
  std::atomic_int errors{0};
  std::atomic_int reads{0};

  auto multi_reader = [&] {
    std::vector<proto::hazard_pointer> hps;
    hps.reserve(kWriters);
    for (int i = 0; i < kWriters; ++i)
      hps.emplace_back(make_hazard_pointer());
    while (!stop.load(std::memory_order_acquire)) {
      for (int i = 0; i < kWriters; ++i) {
        const QuarantinedNode* const p = hps[i].protect(srcs[i]);
        if (p) {
          reads.fetch_add(1, std::memory_order_relaxed);
          if (p->value == kPoisoned)
            errors.fetch_add(1, std::memory_order_relaxed);
        }
        hps[i].reset_protection();
      }
    }
  };

  auto single_writer = [&](int idx) {
    int counter = 1;
    while (!stop.load(std::memory_order_acquire) && counter < kMaxNodesPerWriter) {
      QuarantinedNode* next = new QuarantinedNode(++counter);
      QuarantinedNode* old = srcs[idx].exchange(next, std::memory_order_seq_cst);
      if (old)
        old->retire();
    }
    QuarantinedNode* final = srcs[idx].exchange(nullptr, std::memory_order_seq_cst);
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
  EXPECT_EQ(errors.load(), 0) << "reader dereferenced a node the domain had already reclaimed";
  EXPECT_GT(reads.load(), 0) << "no reader ever observed a node; the test proved nothing";
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
  // The one test that keeps the real operator delete path under readers, so the
  // sanitizers have genuine freed memory to catch: a failed protection shows up
  // as heap-use-after-free under ASan, or as a race under TSan.  There is no
  // value check here because no sentinel can survive the free -- see the note
  // on the quarantine above.
  //
  // What this can and cannot prove on x86, since a green run here is easy to
  // over-read:  it is a sampling test, so it can only ever produce weak
  // positive evidence.  Note the reason it is weak is *not* "x86 is TSO, so the
  // reordering cannot happen" -- that is false.  TSO permits exactly the
  // StoreLoad reordering this bug needs, and the litmus shape does reproduce on
  // x86_64 (measured 1 and 127 positives per 10^6 with litmus7).  What
  // plausibly hides it in the *real* code is narrower: the reclaim path's mutex
  // performs locked RMWs between the removal store and the hazard-slot scan.
  // So the honest claim is "unlikely to reproduce on x86, for a reason specific
  // to the lock", and the deterministic evidence lives in tools/litmus/ and in
  // the quarantine tests above, not here.
  constexpr int kReaders = 6;
  std::atomic<Node*> shared{new Node(42)};
  std::atomic<bool> stop{false};
  std::atomic<int> observed{kNothingObserved};

  std::vector<std::thread> threads;
  threads.reserve(kReaders + 1);

  for (int i = 0; i < kReaders; ++i)
    threads.emplace_back(reader_fn, std::ref(shared), std::ref(stop), std::ref(observed));
  threads.emplace_back(writer_fn, std::ref(shared), std::ref(stop));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_release);
  for (auto& t : threads)
    t.join();

  hazptr_default_domain().synchronize();
  EXPECT_NE(observed.load(), kNothingObserved) << "no reader ever observed a node; the test proved nothing";
}
