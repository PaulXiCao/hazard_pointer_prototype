// R2: retire() is noexcept per [saferecl.hp.base], so it must not be able to
// fail. Before the intrusive retire list it pushed onto a std::vector, which
// made OOM inside a noexcept function -- i.e. std::terminate(). This test
// replaces the global allocation functions and asserts that the retire path
// performs no allocation at all.
//
// The test owns the whole process's allocator hooks, so it lives in its own
// binary: counting is armed only around the calls under test, but replacing
// operator new is global and must not perturb other suites.
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <new>
#include <vector>

namespace {
std::atomic<bool> g_armed{false};
std::atomic<int> g_allocations{0};

void note_allocation() noexcept {
  if (g_armed.load(std::memory_order::relaxed))
    g_allocations.fetch_add(1, std::memory_order::relaxed);
}

struct Node : proto::hazard_pointer_obj_base<Node> {
  int value = 0;
};
} // namespace

// Replaced globals. malloc/free are paired consistently across all four, so
// mixing with the untouched aligned overloads is safe.
void* operator new(std::size_t n) {
  note_allocation();
  void* const p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr)
    throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// The counter is only evidence if it can actually count. A green run of the
// test below proves nothing unless the harness is known to fire -- the same
// trap as the original concurrent.cc finding.
TEST(RetireNoAlloc, HarnessDetectsAnAllocation) {
  static std::atomic<void*> sink{nullptr}; // opaque, so the pair cannot be elided

  g_allocations.store(0, std::memory_order::relaxed);
  g_armed.store(true, std::memory_order::relaxed);
  sink.store(::operator new(64), std::memory_order::relaxed);
  g_armed.store(false, std::memory_order::relaxed);
  ::operator delete(sink.exchange(nullptr, std::memory_order::relaxed));

  ASSERT_EQ(g_allocations.load(std::memory_order::relaxed), 1)
      << "the allocation counter never fires, so the no-alloc test below is vacuous";
}

TEST(RetireNoAlloc, RetireIsASpliceAndAllocatesNothing) {
  using proto::detail::hazptr_default_domain;

  // Hold hazard pointers so the auto-synchronize threshold (> 2 * active
  // slots) is not crossed by the retires below. Acquiring slots may allocate;
  // that happens before arming, and make_hazard_pointer() is the one function
  // [saferecl.hp.make]/3 explicitly allows to throw bad_alloc.
  constexpr int kSlots = 4;
  std::vector<proto::hazard_pointer> hps;
  hps.reserve(kSlots);
  for (int i = 0; i < kSlots; ++i)
    hps.push_back(proto::make_hazard_pointer());

  // Drain leftovers and force this thread's retire-list node to register, so
  // neither happens inside the measured region.
  hazptr_default_domain().synchronize();
  ASSERT_EQ(hazptr_default_domain().retire_list_size(), 0u);

  constexpr int kRetires = 2 * kSlots; // threshold is `>`, so this stays under it
  std::vector<Node*> nodes;
  nodes.reserve(kRetires);
  for (int i = 0; i < kRetires; ++i)
    nodes.push_back(new Node()); // Node is an aggregate; () avoids aggregate-initialising the protected base

  g_allocations.store(0, std::memory_order::relaxed);
  g_armed.store(true, std::memory_order::relaxed);
  for (Node* n : nodes)
    n->retire();
  g_armed.store(false, std::memory_order::relaxed);

  EXPECT_EQ(g_allocations.load(std::memory_order::relaxed), 0)
      << "retire() allocated; the retire list is not purely intrusive";
  // Proves the retires actually landed, rather than the count being zero
  // because nothing happened.
  EXPECT_EQ(hazptr_default_domain().retire_list_size(), static_cast<std::size_t>(kRetires));

  hps.clear();
  hazptr_default_domain().synchronize();
}

// The other half of the guarantee: retire() may auto-synchronize, so a
// reclamation that allocates puts the allocation back on retire()'s path.
// Records live in a linked list that the scan walks directly, and the
// protected-set buffer is owned by the thread and reused, so once it has been
// sized a reclamation allocates nothing at all.
TEST(RetireNoAlloc, SteadyStateSynchronizeAllocatesNothing) {
  using proto::detail::hazptr_default_domain;

  constexpr int kSlots = 4;
  std::vector<proto::hazard_pointer> hps;
  hps.reserve(kSlots);
  for (int i = 0; i < kSlots; ++i)
    hps.push_back(proto::make_hazard_pointer());

  // Warm-up: the first scan on this thread sizes the buffer, and the records
  // themselves are created on demand. Both are allowed to allocate -- the
  // claim is about the steady state, so reach it before measuring.
  for (int i = 0; i < 4; ++i)
    (new Node())->retire();
  hazptr_default_domain().synchronize();
  hazptr_default_domain().synchronize();

  std::vector<Node*> nodes;
  nodes.reserve(8);
  for (int i = 0; i < 8; ++i)
    nodes.push_back(new Node());

  g_allocations.store(0, std::memory_order::relaxed);
  g_armed.store(true, std::memory_order::relaxed);
  for (Node* n : nodes)
    n->retire();
  hazptr_default_domain().synchronize();
  g_armed.store(false, std::memory_order::relaxed);

  EXPECT_EQ(g_allocations.load(std::memory_order::relaxed), 0) << "reclamation allocated in the steady state";
  EXPECT_EQ(hazptr_default_domain().retire_list_size(), 0u) << "nothing was actually reclaimed";
}

TEST(RetireNoAlloc, ReclamationStillHappens) {
  using proto::detail::hazptr_default_domain;

  int destroyed = 0;
  struct Counted : proto::hazard_pointer_obj_base<Counted> {
    int* sink;
    explicit Counted(int* s) : sink(s) {}
    ~Counted() { ++*sink; }
  };

  for (int i = 0; i < 3; ++i)
    (new Counted(&destroyed))->retire();
  hazptr_default_domain().synchronize();

  EXPECT_EQ(destroyed, 3);
}
