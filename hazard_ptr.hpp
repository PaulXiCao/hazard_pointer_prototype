#pragma once

// Prototype implementation of C++26 <hazard_pointer>. See PORTING_NOTES.md for
// the transformations required when moving this header into the libstdc++ tree.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

namespace proto {

namespace detail {
// Empty tag privately inherited by hazard_pointer_obj_base<T,D>. Enables the
// HazardProtectable concept to detect T derives from some hazard_pointer_obj_base<T,D>
// without knowing D. std::is_base_of ignores access -- private inheritance suffices.
struct hazard_obj_base_tag {};
} // namespace detail

// --- hazard_pointer_obj_base ------------------------------------------------

// CRTP base for objects safely reclaimed through hazard pointers.
// T must derive from hazard_pointer_obj_base<T[, D]>.
// D is the deleter type; defaults to std::default_delete<T> (i.e. operator delete).
template <class T, class D = std::default_delete<T>>
class hazard_pointer_obj_base : private detail::hazard_obj_base_tag {
public:
  // Submit this to the default domain's retire list.
  // Deletion is deferred until synchronize() confirms no hazard pointer holds this address.
  // pre contracts ([32.11.3.3]/6): x is not already retired; move-assigning d does not throw.
  void retire(D d = D()) noexcept
#ifdef __cpp_contracts
      pre(!retired_) pre(std::is_nothrow_move_assignable_v<D>)
#endif
          ;

protected:
  hazard_pointer_obj_base() = default;                               // NOLINT(bugprone-crtp-constructor-accessibility)
  hazard_pointer_obj_base(const hazard_pointer_obj_base&) = default; // NOLINT(bugprone-crtp-constructor-accessibility)
  hazard_pointer_obj_base(hazard_pointer_obj_base&&) = default;      // NOLINT(bugprone-crtp-constructor-accessibility)
  hazard_pointer_obj_base& operator=(const hazard_pointer_obj_base&) = default;
  hazard_pointer_obj_base& operator=(hazard_pointer_obj_base&&) = default;
  ~hazard_pointer_obj_base() = default;

private:
  D deleter;
#ifdef __cpp_contracts
  bool retired_ = false; // only needed to back the pre contract (x is not retired)
#endif
};

// --- hazard_pointer ---------------------------------------------------------

// RAII handle owning one slot in the default domain.
// Acquired via make_hazard_pointer(). Not copyable; movable.
class hazard_pointer {
public:
  // Construct an empty handle (owns no slot).
  //
  // GCC ICE workaround: the natural spelling is `= default`, but a defaulted constructor combined with a `post()`
  // contract annotation and an in-class definition crashes gcc -fcontracts with an internal compiler error. Reported
  // upstream: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=125403 . Workaround: provide an empty user-defined body
  // instead of `= default`, but only for the affected toolchain (gcc with contracts enabled). All other configurations
  // keep `= default`. Behavior is equivalent here because slot_ has a NSDMI (= nullptr), so value-init of the empty
  // body and defaulted init produce the same object state. Revisit once the gcc fix lands; restore `= default`
  // unconditionally then.
#if defined(__cpp_contracts) && defined(__GNUC__) && !defined(__clang__)
  hazard_pointer() noexcept post(empty()) {} // gcc ICE workaround (PR125403)
#elif defined(__cpp_contracts)
  hazard_pointer() noexcept post(empty()) = default;
#else
  hazard_pointer() noexcept = default;
#endif

  // Transfer slot ownership from o. o becomes empty.
  // post captures only: o is empty after move. The conditional half --
  // "*this is empty iff o was empty before" -- requires old-value capture,
  // not available in C++26 contracts (P2900 deferred).
  hazard_pointer(hazard_pointer&& other) noexcept
#ifdef __cpp_contracts
      post(other.empty())
#endif
          ;

  // Release current slot (if any), then take ownership of other's slot.
  // No post() contract: all postconditions depend on pre-call state of other / *this;
  // old-value capture not available in C++26 contracts (P2900 deferred).
  hazard_pointer& operator=(hazard_pointer&& other) noexcept;

  // Clear protection and release the slot back to the domain.
  ~hazard_pointer();

  hazard_pointer(const hazard_pointer&) = delete;
  hazard_pointer& operator=(const hazard_pointer&) = delete;

  // True if this handle owns no hazard pointer (slot_ == nullptr).
  // A handle that owns an unassociated hazard pointer (slot holds nullptr) is NOT empty.
  [[nodiscard]] bool empty() const noexcept;

  // Protect src: retry try_protect until it succeeds. Returns the protected pointer.
  // ABA safety is provided by try_protect's seq_cst-store + seq_cst-reload pattern, not
  // by the retry loop. The loop exists solely to ensure convergence when src changes.
  template <class T>
  [[nodiscard]] T* protect(const std::atomic<T*>& src) noexcept;

  // Single-attempt protect: publish hazard for ptr, re-check that src still holds ptr.
  // On success (src unchanged): slot holds ptr, returns true.
  // On failure (src changed): clears slot, writes new src value into ptr, returns false.
  // The seq_cst hazard store and seq_cst src reload form an SC pair: either synchronize()'s
  // snapshot observes the hazard or the reload observes the new src value.
  template <class T>
  [[nodiscard]] bool try_protect(T*& ptr, const std::atomic<T*>& src) noexcept
#ifdef __cpp_contracts
      pre(!empty())
#endif
          ;

  // Publish a hazard for a pointer already held by the caller.
  // Unlike protect(), performs no atomic re-read of a source -- caller is responsible
  // for ensuring p was loaded with correct ordering before calling.
  template <class T>
  void reset_protection(const T* ptr) noexcept
#ifdef __cpp_contracts
      pre(!empty())
#endif
          ;

  // Clear the slot: store nullptr (release ordering).
  // After this call, the previously protected object may be reclaimed by synchronize().
  void reset_protection(std::nullptr_t = nullptr) noexcept
#ifdef __cpp_contracts
      pre(!empty()) post(slot_->load(std::memory_order::relaxed) == nullptr)
#endif
          ;

  // Exchange slots with other.
  void swap(hazard_pointer& other) noexcept;

private:
  std::atomic<void*>* slot_ = nullptr; // pointer into HazardDomain::slots_; null = empty handle

  explicit hazard_pointer(std::atomic<void*>* slot) noexcept;

  friend hazard_pointer make_hazard_pointer();
};

// --- Internal domain (not part of std API) ----------------------------------
namespace detail {

// Approximation of "hazard-protectable type" ([32.11.3.1]/2).
// Checks: T is a class that derives from some hazard_pointer_obj_base<T,D> (for any D),
//         detected via the private hazard_obj_base_tag base (std::is_base_of ignores access).
// Not checked here (D unknown -- verified in retire() where D is explicit):
//   - hazard_pointer_obj_base<T,D> is a PUBLIC base of T
//   - hazard_pointer_obj_base<T,D> is a NON-VIRTUAL base of T
//   - exactly one such base exists (no other hazard_pointer_obj_base<T2,D2>)
template <class T>
concept HazardProtectable = std::is_class_v<T> && std::is_base_of_v<hazard_obj_base_tag, T>;

using deleter_fn = void (*)(void*);

// Type-erased retirement entry: pointer + deleter queued for reclamation.
struct RetireRecord {
  void* ptr = nullptr;
  deleter_fn deleter = nullptr;
};

struct RetireListNode;

// One hazard slot padded to a full cache line so adjacent slots in slots_[] do not
// share a cache line -- prevents false sharing between threads using different slots.
// GCC warns that hardware_destructive_interference_size can vary with -mcpu/-mtune,
// which would be an ABI hazard in a shared-library header. Safe to suppress here:
// this is a single-TU prototype with no cross-TU ABI boundary on PaddedSlot.
// In libstdc++ proper, use __GCC_DESTRUCTIVE_SIZE (compiler built-in, no warning).
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option" // must come first: suppresses the next line if unknown
#pragma clang diagnostic ignored "-Winterference-size"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
struct alignas(std::hardware_destructive_interference_size) PaddedSlot {
  std::atomic<void*> value{nullptr};
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Owns the hazard slot pool and the pending retire list.
// Process-global singleton; construction and destruction restricted to hazptr_default_domain().
class HazardDomain {
public:
  // Claim a free slot. Grows the pool if all existing slots are occupied.
  // Throws std::bad_alloc only on OOM.
  [[nodiscard]] std::atomic<void*>* acquire_slot();

  // Return a slot to the pool.
  void release_slot(std::atomic<void*>* slot);

  // Append {ptr, deleter} to the retire list.
  // Auto-synchronizes when list size exceeds 2 * active_count_.
  void retire_impl(void* ptr, deleter_fn deleter);

  // Snapshot all active slots -> protected set.
  // Reclaim every retired object whose pointer is not in the protected set.
  void synchronize();

  // Number of currently acquired slots. Exposed for testing.
  [[nodiscard]] std::size_t active_slots() const noexcept;

  // Number of entries in the calling thread's retire list; exposed for single-thread testing.
  [[nodiscard]] std::size_t retire_list_size() const noexcept;

private:
  static constexpr std::size_t kInitialSlots = 8; // starting pool size; grows on demand

  // slot_free_[] and slots_[] are kept separate: slot_free_[] is a compact bool vector
  // (1 cache line per 64 slots) so acquire_slot() scans it cheaply. Merging the free flag
  // into PaddedSlot would force 1 cache-line miss per slot during acquisition.
  //
  // slots_[] uses unique_ptr so that PaddedSlot objects (and the &PaddedSlot::value pointers
  // held by live hazard_pointer objects) are never invalidated when the vector grows.
  // The vector's internal pointer array may be reallocated; the PaddedSlots themselves
  // live at stable heap addresses for their entire lifetime.
  std::vector<bool> slot_free_; // guarded by slot_free_mutex_; true = available
  std::mutex slot_free_mutex_;
  std::vector<std::unique_ptr<PaddedSlot>> slots_; // structure guarded by slot_free_mutex_; PaddedSlot addresses stable
  std::atomic_size_t active_count_ = 0;            // atomically maintained; mirrors slot_free_ occupancy

  // Retired records from threads that exited with still-protected survivors.
  // Collected by synchronize() alongside live-thread lists.
  std::vector<RetireRecord> orphan_list_; // guarded by orphan_mutex_
  std::mutex orphan_mutex_;

  RetireListNode* retire_lists_head_ = nullptr; // guarded by retire_lists_mutex_
  std::size_t retire_list_node_count_ = 0;      // guarded by retire_lists_mutex_
  std::mutex retire_lists_mutex_;

  void ensure_node_registered();

  friend struct RetireListNode;
  void unregister_node(const RetireListNode& node) noexcept;

  // Private ctor/dtor + friend hazptr_default_domain() enforces at compile time that the
  // only HazardDomain instance is the static local in hazptr_default_domain() -- static
  // storage duration is therefore guaranteed by construction, not a runtime check.
  // The friend declaration also covers the static-local destructor: the compiler
  // registers the atexit handler from within the friend function's scope, where
  // the private destructor is accessible.
  HazardDomain();
  ~HazardDomain();
  friend HazardDomain& hazptr_default_domain() noexcept;
};

// Process-wide default domain. Lazily initialized on first call.
inline HazardDomain& hazptr_default_domain() noexcept {
  static HazardDomain domain;
  return domain;
}

// Per-thread node in the retire_lists_ linked list.
// Each thread that calls retire_impl() or synchronize() has exactly one node,
// registered lazily on first use and removed when the thread exits.
// list_mutex guards *list against concurrent access between synchronize() (any thread)
// and retire_impl() (the owning thread). retire_lists_mutex_ guards the list structure
// (next pointers, retire_lists_head_) but NOT the contents of *list.
struct RetireListNode {
  std::vector<RetireRecord>* list = nullptr; // points to this thread's retire list; null = not yet registered
  std::mutex list_mutex;                     // guards *list: synchronize() vs retire_impl() on owning thread
  RetireListNode* next = nullptr;            // intrusive singly-linked list; guarded by retire_lists_mutex_

  // Called on thread exit. Drains the retire list, offloads survivors, then unregisters.
  ~RetireListNode();
};

// Bundles a thread's retire list and its registry node into one object so that
// C++ member-destruction order (reverse of declaration) guarantees retire_list
// outlives node -- i.e. ~RetireListNode() always runs before retire_list is freed.
struct ThreadState {
  std::vector<RetireRecord> retire_list; // destroyed second (last)
  RetireListNode node;                   // destroyed first -> ~RetireListNode() sees a live retire_list
};
inline thread_local ThreadState tl_state_; // external linkage -- one instance per thread across all TUs
} // namespace detail

// --- Free functions ---------------------------------------------------------

inline void swap(hazard_pointer& a, hazard_pointer& b) noexcept { a.swap(b); }

// Acquire a slot from the default domain and return an owning handle.
[[nodiscard]] inline hazard_pointer make_hazard_pointer() {
  return hazard_pointer(detail::hazptr_default_domain().acquire_slot());
}

// --- hazard_pointer_obj_base implementation ---------------------------------

template <typename T, typename D>
inline void hazard_pointer_obj_base<T, D>::retire(D d) noexcept {
  // noexcept per std API. retire_impl() may throw (mutex::lock, vector::push_back).
  // Any exception escaping this boundary calls std::terminate().

  // [32.11.3.3]/1: D shall be a function object type for which d(ptr) is valid (T* ptr).
  static_assert(std::is_invocable_v<D, T*>, "D must be invocable with T* -- [32.11.3.3]/1");
  // [32.11.3.3]/3: D shall meet Cpp17DefaultConstructible and Cpp17MoveAssignable.
  static_assert(std::is_default_constructible_v<D>, "D must be default-constructible -- [32.11.3.3]/3");
  static_assert(std::is_move_assignable_v<D>, "D must be move-assignable -- [32.11.3.3]/3");
  // [32.11.3.3]/5 (Mandates): T is a hazard-protectable type.
  static_assert(std::is_base_of_v<hazard_pointer_obj_base<T, D>, T>,
                "T must derive from hazard_pointer_obj_base<T, D> -- [32.11.3.3]/5");
  static_assert(std::is_convertible_v<T*, hazard_pointer_obj_base<T, D>*>,
                "hazard_pointer_obj_base<T, D> must be a public base of T -- [32.11.3.3]/5");
#ifdef __cpp_lib_is_virtual_base_of
  static_assert(!std::is_virtual_base_of_v<hazard_pointer_obj_base<T, D>, T>,
                "hazard_pointer_obj_base<T, D> must be a non-virtual base of T -- [32.11.3.3]/5");
#endif
#ifdef __cpp_contracts
  retired_ = true; // mark before retire_impl so any recursive synchronize() sees the correct state
#endif

  deleter = std::move(d);

  // D is the first data member of this base class. hazard_pointer_obj_base has one empty
  // private base (hazard_obj_base_tag) which contributes zero bytes via EBO, so D still
  // sits at offset 0 of the base subobject. The base subobject itself sits at offset 0 of T
  // (single non-virtual inheritance). So the T* and the D* share an address -- the lambda
  // can recover the deleter via a cast to hazard_pointer_obj_base*.
  static_assert(offsetof(hazard_pointer_obj_base, deleter) == 0,
                "D deleter must be at offset 0 for the retire() cast to be valid");

  T* self = static_cast<T*>(this);
  detail::hazptr_default_domain().retire_impl(static_cast<void*>(self), [](void* p) {
    T* self_ = static_cast<T*>(p);
    auto* base = static_cast<hazard_pointer_obj_base*>(self_);
    D d = std::move(base->deleter);
    d(self_);
  });
}

// --- hazard_pointer implementation ------------------------------------------

inline hazard_pointer::hazard_pointer(std::atomic<void*>* slot) noexcept : slot_(slot) {}

inline hazard_pointer::~hazard_pointer() {
  if (slot_) {
    reset_protection();
    detail::hazptr_default_domain().release_slot(slot_);
  }
}

inline hazard_pointer::hazard_pointer(hazard_pointer&& other) noexcept : slot_(other.slot_) { other.slot_ = nullptr; }

inline hazard_pointer& hazard_pointer::operator=(hazard_pointer&& other) noexcept {
  if (this == &other)
    return *this;
  if (!empty()) {
    reset_protection();
    detail::hazptr_default_domain().release_slot(slot_);
  }
  slot_ = other.slot_;
  other.slot_ = nullptr;
  return *this;
}

inline bool hazard_pointer::empty() const noexcept {
  return slot_ == nullptr; // empty = owns no hazard pointer; unassociated (slot holds nullptr) is not empty
}

template <typename T>
inline bool hazard_pointer::try_protect(T*& ptr, const std::atomic<T*>& src) noexcept {
  static_assert(detail::HazardProtectable<T>, "T must be a hazard-protectable type -- [32.11.3.4.3]/3");
  const T* const old = ptr;
  reset_protection(old);
  ptr = src.load(std::memory_order::seq_cst);
  if (!(old == ptr)) // same expression as in std
    reset_protection();
  return old == ptr;
}

template <typename T>
inline T* hazard_pointer::protect(const std::atomic<T*>& src) noexcept {
  static_assert(detail::HazardProtectable<T>, "T must be a hazard-protectable type -- [32.11.3.4.3]/3");
  T* ptr = src.load(std::memory_order::relaxed); // NOLINT(misc-const-correctness)
  while (!try_protect(ptr, src)) {
    ; // retry
  }
  return ptr;
}

template <typename T>
inline void hazard_pointer::reset_protection(const T* ptr) noexcept {
  static_assert(detail::HazardProtectable<T>, "T must be a hazard-protectable type -- [32.11.3.4.3]/7");
  if (ptr == nullptr)
    reset_protection();
  else if (slot_)
    slot_->store(const_cast<void*>(static_cast<const void*>(ptr)), std::memory_order::seq_cst);
}

inline void hazard_pointer::reset_protection(std::nullptr_t) noexcept {
  if (slot_) {
    slot_->store(nullptr, std::memory_order::release);
  }
}

inline void hazard_pointer::swap(hazard_pointer& other) noexcept { std::swap(slot_, other.slot_); }

// --- HazardDomain implementation --------------------------------------------
namespace detail {
inline HazardDomain::HazardDomain() : slot_free_(kInitialSlots, true) {
  slots_.reserve(kInitialSlots);
  for (std::size_t i = 0; i < kInitialSlots; ++i)
    slots_.push_back(std::make_unique<PaddedSlot>());
}

inline HazardDomain::~HazardDomain() {
  // Called during static-storage destruction (program exit). Thread-local storage is destroyed
  // before static storage, so tl_state_ is already gone -- synchronize() cannot be called.
  // All threads have exited, so no hazard pointers are held; the protected-set check is
  // unnecessary and every orphan record can be deleted unconditionally.
  for (const RetireRecord& r : orphan_list_)
    r.deleter(r.ptr);
}

inline std::atomic<void*>* HazardDomain::acquire_slot() {
  const std::lock_guard _(slot_free_mutex_);
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slot_free_[i]) {
      slot_free_[i] = false;
      ++active_count_;
      return &slots_[i]->value;
    }
  }
  // All slots occupied -- grow the pool by one.
  slot_free_.push_back(false);
  slots_.push_back(std::make_unique<PaddedSlot>()); // throws std::bad_alloc on OOM
  ++active_count_;
  return &slots_.back()->value;
}

inline void HazardDomain::release_slot(std::atomic<void*>* slot) {
  slot->store(nullptr, std::memory_order::release); // clear hazard before returning slot to free pool
  const std::lock_guard _(slot_free_mutex_);
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (&slots_[i]->value == slot) {
      slot_free_[i] = true;
      --active_count_;
      return;
    }
  }
#ifdef __cpp_contracts
  contract_assert(false);
#else
  assert(false && "HazardDomain: slot not found");
#endif
}

inline std::size_t HazardDomain::active_slots() const noexcept {
  return active_count_.load(std::memory_order::relaxed);
}

inline std::size_t HazardDomain::retire_list_size() const noexcept { return tl_state_.retire_list.size(); }

inline void HazardDomain::retire_impl(void* ptr, deleter_fn deleter) {
  // Lazy registration: on the first retire_impl() call per thread, insert tl_state_.node into
  // the global retire_lists_ linked list. node.list == nullptr serves as the "not yet
  // registered" sentinel -- safe to check without a lock because only the owning thread
  // ever writes this field (and only once, here).
  ensure_node_registered();

  // Push under list_mutex so synchronize() cannot observe a partially-grown vector.
  const std::size_t sz = [&] {
    const std::lock_guard _(tl_state_.node.list_mutex);
    tl_state_.retire_list.push_back({ptr, deleter});
    return tl_state_.retire_list.size();
  }();

  // Threshold is a heuristic: trigger a scan when the retire list grows to more than
  // twice the number of active hazard pointers.
  if (sz > 2 * active_count_.load(std::memory_order::relaxed))
    synchronize(); // important: called without lock
}

// Called from ~RetireListNode() when a thread exits. Splices the node out of the
// retire_lists_ linked list so synchronize() never dereferences its (soon-to-be-invalid)
// list pointer. Must complete before the thread's retire_list storage is freed.
inline void HazardDomain::unregister_node(const RetireListNode& node) noexcept {
  const std::lock_guard _(retire_lists_mutex_);
  RetireListNode *curr = retire_lists_head_, *prev = nullptr;
  while (curr && curr != &node) {
    prev = curr;
    curr = curr->next;
  }
  if (!curr) // node was never registered (ensure_node_registered never ran)
    return;
  if (prev)
    prev->next = curr->next; // unlink from middle or tail
  else
    retire_lists_head_ = curr->next; // unlink from head
  --retire_list_node_count_;
}

inline void append_to(std::vector<RetireRecord>& dst, const std::vector<RetireRecord>& src) {
#ifdef __cpp_lib_containers_ranges
  dst.append_range(src);
#else
  dst.insert(dst.end(), src.begin(), src.end());
#endif
}

inline void HazardDomain::synchronize() {
  // Ensure this thread is registered so survivors have a valid home to return to.
  // Mirrors the registration in retire_impl(); safe to call from either path.
  ensure_node_registered();

  // Step 1: collect -- atomically swap every thread's retire list with an empty one,
  // then drain the orphan list. Done BEFORE snapshot so that any reader publishing
  // a hazard concurrently with the collect step sees the object still in src (and
  // thus retries) OR has the hazard visible in the snapshot below.
  std::vector<RetireRecord> all_pending;
  {
    std::vector<std::vector<RetireRecord>> tmp;
    tmp.reserve(active_count_.load(std::memory_order::relaxed)); // hint
    {
      const std::lock_guard _(retire_lists_mutex_);
      tmp.reserve(retire_list_node_count_);
      for (RetireListNode* n = retire_lists_head_; n; n = n->next) {
        tmp.emplace_back();
        const std::lock_guard _(n->list_mutex);
        std::swap(tmp.back(), *n->list);
      }
    }
    const std::size_t sz = std::ranges::fold_left(
        tmp, std::size_t{0}, [](std::size_t sz, const std::vector<RetireRecord>& x) { return sz + x.size(); });
    all_pending.reserve(sz);
    for (const std::vector<RetireRecord>& t : tmp) {
      append_to(all_pending, t);
    }
  }
  {
    const std::lock_guard _(orphan_mutex_);
    append_to(all_pending, orphan_list_);
    orphan_list_.clear();
  }

  // Step 2: snapshot slot addresses under slot_free_mutex_ for a consistent view of slots_.
  // Actual acquire loads happen after releasing the lock.
  const std::size_t active_hint = active_count_.load(std::memory_order::relaxed);
  std::vector<std::atomic<void*>*> slot_ptrs;
  slot_ptrs.reserve(active_hint);
  {
    const std::lock_guard _(slot_free_mutex_);
    slot_ptrs.reserve(slots_.size());
    for (const std::unique_ptr<PaddedSlot>& s : slots_)
      slot_ptrs.push_back(&s->value);
  }
  std::vector<void*> snapshot_slots;
  snapshot_slots.reserve(slot_ptrs.size());
  for (const std::atomic<void*>* sp : slot_ptrs) {
    if (void* ptr = sp->load(std::memory_order::acquire)) // NOLINT(misc-const-correctness)
      snapshot_slots.push_back(ptr);
  }
  std::ranges::sort(snapshot_slots);

  // Step 3: reclaim -- delete every record not in the protected set.
  // No lock held here; deleters can safely call retire() or even synchronize().
  // snapshot_slots is sorted above; binary_search is O(log n) per record.
  std::vector<RetireRecord> survivors;
  survivors.reserve(all_pending.size());
  for (const RetireRecord& r : all_pending) {
    if (std::ranges::binary_search(snapshot_slots, r.ptr))
      survivors.push_back(r);
    else {
      r.deleter(r.ptr);
    }
  }

  // Step 4: put survivors back into the calling thread's own list.
  // Survivors cannot be returned to their original threads because those threads may
  // have exited between the collect and here. The calling thread's list is guaranteed
  // alive for the duration of this call.
  if (!survivors.empty()) {
    const std::lock_guard _(tl_state_.node.list_mutex);
    append_to(tl_state_.retire_list, survivors);
  }
}

inline void HazardDomain::ensure_node_registered() {
  thread_local bool registered = false;
  if (registered)
    return;
  tl_state_.node.list = &tl_state_.retire_list;
  {
    const std::lock_guard _(retire_lists_mutex_);
    tl_state_.node.next = retire_lists_head_;
    retire_lists_head_ = &tl_state_.node;
    ++retire_list_node_count_;
  }
  registered = true; // set only after successful registration
}

inline RetireListNode::~RetireListNode() {
  if (!list)
    return; // ensure_node_registered never ran -- nothing to do

  HazardDomain& domain = hazptr_default_domain();

  // Reclaim as much as possible before this thread's retire list goes away.
  // synchronize() puts survivors back into tl_state_.retire_list (still alive here
  // because ThreadState destroys node before retire_list).
  domain.synchronize();

  // Unregister before touching the retire list without list_mutex.
  // unregister_node acquires retire_lists_mutex_, which synchronize() holds for its
  // entire collect loop -- so this call blocks until any concurrent synchronize() that
  // has already seen this node has fully released both retire_lists_mutex_ and
  // list_mutex.  After it returns, no future synchronize() can find this node, so
  // list is exclusively owned by this thread.
  domain.unregister_node(*this);

  // Move any remaining survivors (still actively protected) to the domain's orphan list
  // so a future synchronize() from any thread can eventually reclaim them.
  if (!list->empty()) {
    const std::lock_guard _(domain.orphan_mutex_);
    append_to(domain.orphan_list_, *list);
    list->clear();
  }
}
} // namespace detail

} // namespace proto
