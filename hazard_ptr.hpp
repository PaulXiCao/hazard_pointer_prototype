#pragma once

// Prototype implementation of C++26 <hazard_pointer>. See PORTING_NOTES.md for
// the transformations required when moving this header into the libstdc++ tree.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

// MSVC accepts the standard [[no_unique_address]] but ignores it for ABI
// compatibility; its own spelling is the one that works. Without this, the
// empty default_delete member costs 8 bytes of padding in every protectable
// object and eats the space reserved below. libstdc++ has
// _GLIBCXX_NO_UNIQUE_ADDRESS for this -- see PORTING_NOTES.md.
#if defined(_MSC_VER) && !defined(__clang__)
#define PROTO_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define PROTO_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace proto {

class hazard_pointer;

namespace detail {

struct HazptrObj;

// Type-erased reclamation: invokes the object's deleter on the object.
using reclaim_fn = void (*)(HazptrObj*);

// Non-template private base of hazard_pointer_obj_base<T,D>.
//
// Carries the intrusive retire link and the erased reclaim function, so
// retire() is a pointer splice into a per-thread list and cannot allocate.
// [saferecl.hp.base] declares retire() noexcept, so an allocating retire turns
// OOM into terminate(); putting the list storage in the object is the only way
// out, because a per-thread side table has to be grown by retire() itself.
//
// It also carries the space P2530R3 1.5 ("Guidance for ABI-Stability")
// reserves for the two extensions the paper expects. That reservation is a
// one-way door: hazard_pointer_obj_base is a standard-specified type that
// users *derive from*, so its size is baked into user binaries, and
// libstdc++'s own doc/xml/manual/abi.xml lists changing the layout of a
// standard-specified type as a prohibited change. The reserved members must
// also be *initialised* here, for the same reason: the constructor is inlined
// into user code, so code compiled before a future extension would otherwise
// leave them uninitialised. The corresponding *check* in retire() can be added
// later, since nothing today can set a non-null cohort.
//
// Doubles as the tag the HazardProtectable concept detects: std::is_base_of
// ignores access, so one private base does both jobs.
struct HazptrObj {
  // next == this means "not retired"; every constructor re-establishes it.
  // Backs the [saferecl.hp.base]/6 precondition "x is not retired" in every
  // build rather than only under contracts, and costs nothing -- the link has
  // to be there anyway. Folly's hazptr_obj uses the same sentinel.
  HazptrObj* next = this;
  reclaim_fn reclaim = nullptr;

  // RESERVED for P2530R3 1.5. Never read or written by this implementation.
  void* cohort = nullptr;  // 1.5 item 2: cohort-based synchronous reclamation
  std::uint64_t count = 0; // 1.5 item 1: integrated commutative counting

  // Phrased negatively because both call sites assert the negative:
  // `pre(not_retired())` rather than `pre(!retired())`, where a lone `!` inside
  // a contract annotation is easy to read past.
  [[nodiscard]] bool not_retired() const noexcept { return next == this; }

  HazptrObj() noexcept = default;

  // A copy is a new, unretired object, so the retirement state must not be
  // copied -- otherwise retiring the copy would look like a double retire.
  // This is what costs hazard_pointer_obj_base its trivial copyability, and
  // there is no layout that is both trivially copyable and correct: a trivial
  // copy necessarily copies the state. Folly makes the same trade.
  HazptrObj(const HazptrObj&) noexcept {}
  HazptrObj(HazptrObj&&) noexcept {}
  // Assignment leaves the retirement state alone: assigning to an object does
  // not retire or un-retire it. Deliberately a no-op, so self-assignment needs
  // no special case -- hence the suppressions.
  HazptrObj& operator=(const HazptrObj&) noexcept { // NOLINT(bugprone-unhandled-self-assignment)
    return *this;
  }
  HazptrObj& operator=(HazptrObj&&) noexcept { // NOLINT(bugprone-unhandled-self-assignment)
    return *this;
  }
  ~HazptrObj() = default;
};

// Intrusive singly-linked list of HazptrObj, with O(1) splice.
//
// head/tail rather than head alone: synchronize() concatenates every thread's
// list into one chain, and without a tail pointer each concatenation would walk
// the list it is appending to.
//
// Replacing the per-thread std::vector with this is not the performance
// regression it looks like:
//   - retire() gets cheaper and, more importantly, predictable: two stores and
//     an increment, with no reallocation and no amortisation spike.
//   - synchronize()'s collect step goes from copying every record into one
//     growing vector to O(1) pointer splices.
//   - the scan is roughly neutral. It walks obj->next instead of a contiguous
//     record array, but reclaiming an object already touches its first cache
//     line (the destructor and operator delete both do), so those misses were
//     going to be paid anyway; the vector was an extra array on top of them.
// The real cost is memory, and it is worth being explicit about: the old
// version spent 16 bytes per *retired* object, this one spends 32 bytes per
// *protectable* object whether or not it is ever retired. That is the trade
// P2530R3 1.5 and Folly both take, and it is not optional here -- an allocating
// retire() cannot honour the noexcept in [saferecl.hp.base].
// Numbers rather than reasoning: PLAN step 4's microbenchmark.
struct RetireList {
  HazptrObj* head = nullptr;
  HazptrObj* tail = nullptr;
  std::size_t size = 0; // O(1); retire_impl()'s threshold needs it

  // Tests head rather than size: head is the structural invariant the traversal
  // in synchronize() actually relies on, whereas size is a cache maintained
  // alongside it purely for retire_impl()'s threshold. If the two ever
  // disagree, believing head fails safe.
  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

  void push(HazptrObj* obj) noexcept {
    obj->next = head;
    if (empty())
      tail = obj;
    head = obj;
    ++size;
  }

  // Move every node of other to the front of *this; other becomes empty.
  // Named for the ownership transfer (as std::list::splice is), not for the
  // position: "prepend" would not convey that other is left empty. Order is
  // irrelevant here -- the scan visits every node regardless.
  void splice(RetireList& other) noexcept {
    if (other.empty())
      return;
    if (empty()) {
      head = other.head;
      tail = other.tail;
    } else {
      other.tail->next = head;
      head = other.head;
    }
    size += other.size;
    other.clear();
  }

  void clear() noexcept {
    head = tail = nullptr;
    size = 0;
  }

  // Detach the whole list, leaving *this empty.
  [[nodiscard]] RetireList take() noexcept { return std::exchange(*this, RetireList{}); }
};

// One hazard pointer record, padded to a full cache line so that records owned
// by different threads do not share one -- prevents false sharing.
// GCC warns that hardware_destructive_interference_size can vary with -mcpu/-mtune,
// which would be an ABI hazard in a shared-library header. Safe to suppress here:
// this is a single-TU prototype with no cross-TU ABI boundary on HazptrRec.
// In libstdc++ proper, use __GCC_DESTRUCTIVE_SIZE (compiler built-in, no warning).
//
// This is the "internal structure associated with the actual hazard pointers"
// of P2530R3 1.5 item 3. The paper is explicit that the domain pointer belongs
// here rather than in hazard_pointer, and gives the reason: keeping the handle
// one word wide is what buys the ~4ns construction/destruction of 3.1. Folly
// agrees -- hazptr_holder is a single hazptr_rec*.
//
// Records are never destroyed before the domain is, and are never unlinked.
// That is what lets synchronize() walk the list with plain atomic loads and no
// lock, and what keeps the address a live hazard_pointer holds stable.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option" // must come first: suppresses the next line if unknown
#pragma clang diagnostic ignored "-Winterference-size"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
struct alignas(std::hardware_destructive_interference_size) HazptrRec {
  // The hazard pointer proper. Holds a HazptrObj* (see reset_protection) or null.
  std::atomic<void*> hazard{nullptr};

  // Claimed by a live hazard_pointer? Released with a plain store, claimed with
  // a CAS, so neither path needs the allocation mutex.
  std::atomic<bool> active{false};

  // Written once, before the record is published; read by every scan.
  HazptrRec* next = nullptr;

  // RESERVED for P2530R3 1.5 item 3 (custom domains). Null means the default
  // domain, which is the representation the paper suggests, so a future
  // ~hazard_pointer can test it without changing the layout again. Reserve and
  // initialise now, check later -- same argument as HazptrObj's reserved
  // members, except that this one is not user-visible, so only libstdc++'s own
  // inlined code is at stake.
  void* domain = nullptr;
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
} // namespace detail

// --- hazard_pointer_obj_base ------------------------------------------------

// CRTP base for objects safely reclaimed through hazard pointers.
// T must derive from hazard_pointer_obj_base<T[, D]>.
// D is the deleter type; defaults to std::default_delete<T> (i.e. operator delete).
template <class T, class D = std::default_delete<T>>
class hazard_pointer_obj_base : private detail::HazptrObj {
public:
  // Splice this onto the calling thread's retire list in the default domain.
  // Deletion is deferred until synchronize() confirms no hazard pointer holds this address.
  // pre contracts ([32.11.3.3]/6): x is not already retired; move-assigning d does not throw.
  void retire(D d = D()) noexcept
#ifdef __cpp_contracts
      pre(not_retired()) pre(std::is_nothrow_move_assignable_v<D>)
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
  // [[no_unique_address]] is what pays for the space HazptrObj reserves: with a
  // stateless deleter (the default_delete case) it keeps the empty member out
  // of the object's size instead of costing a full aligned word.
  PROTO_NO_UNIQUE_ADDRESS D deleter;

  // reset_protection() has to map a T* to the HazptrObj subobject that
  // synchronize()'s scan compares against, and that base is private. Folly
  // makes its equivalent base public instead; private plus a friend keeps the
  // implicit T* -> HazptrObj* conversion out of user overload resolution.
  friend class hazard_pointer;
};

namespace detail {
// Layout tripwire. hazard_pointer_obj_base is a standard-specified type that
// users derive from, so its size is baked into user binaries and cannot be
// changed once shipped -- libstdc++'s doc/xml/manual/abi.xml lists that as a
// prohibited change. The reserved members exist precisely so the size does not
// have to move later; pin it here so an accidental change fails the build
// rather than the field. Guarded on 8-byte pointers so 32-bit targets are not
// held to a 64-bit number.
struct AbiProbe : hazard_pointer_obj_base<AbiProbe> {};
static_assert(sizeof(void*) != 8 || sizeof(hazard_pointer_obj_base<AbiProbe>) == 32,
              "hazard_pointer_obj_base layout changed -- this is an ABI break, not a refactor");
static_assert(sizeof(void*) != 8 || alignof(hazard_pointer_obj_base<AbiProbe>) == 8,
              "hazard_pointer_obj_base alignment changed -- this is an ABI break");

// The pinned number is per-ABI, so it is guarded on LP64 rather than
// generalised: any formula portable enough to hold on every ABI would have to
// be derived from the members, which would make the assertion follow the code
// instead of pinning it. LP64 is what libstdc++ ships on for every target this
// prototype is exercised on, including the ubuntu-24.04-arm CI runner. A
// 32-bit pin would need a cross-compiled (-m32) job; GitHub offers no 32-bit
// runner.
} // namespace detail

// --- hazard_pointer ---------------------------------------------------------

// RAII handle owning one hazard pointer record in the default domain.
// Acquired via make_hazard_pointer(). Not copyable; movable.
class hazard_pointer {
public:
  // Construct an empty handle (owns no slot).
  //
  // GCC ICE workaround: the natural spelling is `= default`, but a defaulted constructor combined with a `post()`
  // contract annotation and an in-class definition crashes gcc -fcontracts with an internal compiler error. Reported
  // upstream: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=125403 . Workaround: provide an empty user-defined body
  // instead of `= default`, but only for the affected toolchain (gcc with contracts enabled). All other configurations
  // keep `= default`. Behavior is equivalent here because rec_ has a NSDMI (= nullptr), so value-init of the empty
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

  // True if this handle owns no hazard pointer (rec_ == nullptr).
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

  // Clear the hazard: store nullptr (release ordering).
  // After this call, the previously protected object may be reclaimed by synchronize().
  void reset_protection(std::nullptr_t = nullptr) noexcept
#ifdef __cpp_contracts
      pre(!empty()) post(rec_->hazard.load(std::memory_order::relaxed) == nullptr)
#endif
          ;

  // Exchange records with other.
  void swap(hazard_pointer& other) noexcept;

private:
  // One word, and deliberately so: P2530R3 1.5 item 3 puts the reserved domain
  // pointer in the record rather than here, because a future custom-domain
  // extension must not have to grow hazard_pointer -- that would be another
  // prohibited layout change on a standard-specified type. An index would have
  // been the cheaper way to delete release_rec()'s pool scan, but an index
  // cannot name a domain, so it would have bought the scan back at the price of
  // the extension.
  detail::HazptrRec* rec_ = nullptr; // null = empty handle

  explicit hazard_pointer(detail::HazptrRec* rec) noexcept;

  friend hazard_pointer make_hazard_pointer();
};

namespace detail {
// hazard_pointer is frozen for the same reason as hazard_pointer_obj_base, and
// pinned the same way. Staying one word is the whole point of P2530R3 1.5 item
// 3: the reserved domain pointer lives in HazptrRec precisely so that adding
// custom domains later never has to widen this type. Section 3.1's ~4ns
// construction/destruction is the other half of the reason.
static_assert(sizeof(void*) != 8 || sizeof(hazard_pointer) == 8,
              "hazard_pointer must stay one word -- see P2530R3 1.5 item 3");
} // namespace detail

// --- Internal domain (not part of std API) ----------------------------------
namespace detail {

// Approximation of "hazard-protectable type" ([32.11.3.1]/2).
// Checks: T is a class that derives from some hazard_pointer_obj_base<T,D> (for any D),
//         detected via the private HazptrObj base (std::is_base_of ignores access).
// Not checked here (D unknown -- verified in retire() where D is explicit):
//   - hazard_pointer_obj_base<T,D> is a PUBLIC base of T
//   - hazard_pointer_obj_base<T,D> is a NON-VIRTUAL base of T
//   - exactly one such base exists (no other hazard_pointer_obj_base<T2,D2>)
template <class T>
concept HazardProtectable = std::is_class_v<T> && std::is_base_of_v<HazptrObj, T>;

// On the noexcept guarantees below: since C++17 noexcept is part of the
// function *type*, not an attribute, so a declaration and its definition must
// agree or the program is ill-formed. What it does NOT mean is that the body
// cannot throw -- only that std::terminate() is called if it does, and the
// compiler never verifies the body. tests/test_noexcept.cpp checks the declared
// specification (`static_assert(noexcept(expr))`), which is the specification,
// not the behaviour. The behavioural half is clang-tidy's
// bugprone-exception-escape, enabled here via `bugprone-*`; it catches a
// reachable `throw`, but not a call to a merely non-noexcept function such as
// std::mutex::lock, which is exactly the residual hole documented on retire().

struct RetireListNode;

// Owns the hazard pointer records and the pending retire list.
// Process-global singleton; construction and destruction restricted to hazptr_default_domain().
class HazardDomain {
public:
  // Claim an unused record, appending a new one if every existing record is
  // taken. Throws std::bad_alloc only on OOM -- this is the one function on the
  // path from make_hazard_pointer(), which [saferecl.hp.make]/3 explicitly
  // allows to throw. Every allocation this implementation performs on a live
  // domain happens here.
  [[nodiscard]] HazptrRec* acquire_rec();

  // Return a record for reuse. Never allocates, never locks, never fails.
  void release_rec(HazptrRec* rec) noexcept;

  // Splice obj onto the calling thread's retire list.
  // Auto-synchronizes when list size exceeds 2 * active_count_.
  // noexcept: reached from retire(), which [saferecl.hp.base] declares noexcept.
  void retire_impl(HazptrObj* obj) noexcept;

  // Snapshot all hazard records -> protected set.
  // Reclaim every retired object whose pointer is not in the protected set.
  // noexcept: same reason, plus [saferecl.hp.general]/5 makes a throwing
  // deleter undefined behaviour.
  void synchronize() noexcept;

  // Number of currently claimed records. Exposed for testing.
  [[nodiscard]] std::size_t active_slots() const noexcept;

  // Number of entries in the calling thread's retire list; exposed for single-thread testing.
  [[nodiscard]] std::size_t retire_list_size() const noexcept;

private:
  // Head of the append-only record list. Published with release, traversed with
  // acquire; records are never unlinked, so a traversal needs no lock and no
  // snapshot array. The deque and its parallel free bitmap are gone with it:
  // the free flag now lives in the record, which costs nothing because records
  // are cache-line padded anyway, and it is what makes release_rec() O(1)
  // instead of a std::find_if over the whole pool.
  std::atomic<HazptrRec*> recs_head_{nullptr};

  // Serialises appends only. The claim path is a CAS on HazptrRec::active and
  // does not take it; the scan does not take it either.
  std::mutex rec_alloc_mutex_;

  std::atomic_size_t active_count_ = 0; // records currently claimed
  std::atomic_size_t rec_count_ = 0;    // records ever created; sizes the scan buffer

  // Retired objects from threads that exited with still-protected survivors.
  // Collected by synchronize() alongside live-thread lists.
  RetireList orphan_list_; // guarded by orphan_mutex_
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
// list_mutex guards list against concurrent access between synchronize() (any thread)
// and retire_impl() (the owning thread). retire_lists_mutex_ guards the registry
// structure (next pointers, retire_lists_head_) but NOT the contents of list.
//
// The list lives directly in the node now that it is intrusive. The previous
// ThreadState wrapper existed only so that a separately-stored std::vector
// outlived this node during thread exit; three raw pointers need no such
// ordering.
struct RetireListNode {
  RetireList list;                // guarded by list_mutex
  std::mutex list_mutex;          // guards list: synchronize() vs retire_impl() on owning thread
  RetireListNode* next = nullptr; // intrusive registry link; guarded by retire_lists_mutex_
  bool registered = false;        // written only by the owning thread, only once

  // Scratch for synchronize()'s protected set, owned by and reused on this
  // thread. Its capacity only has to grow when the record pool does, so after
  // the first scan a steady-state reclamation allocates nothing at all.
  //
  // It cannot be sized from acquire_rec() the way every other allocation is: a
  // thread that only ever retires never calls make_hazard_pointer(), so it
  // would never reach that path. Growth therefore happens inside a noexcept
  // function and has to be able to fail -- see synchronize().
  std::vector<void*> scan_buf;

  // Called on thread exit. Drains the retire list, offloads survivors, then unregisters.
  ~RetireListNode();
};

inline thread_local RetireListNode tl_node_; // external linkage -- one instance per thread across all TUs
} // namespace detail

// --- Free functions ---------------------------------------------------------

inline void swap(hazard_pointer& a, hazard_pointer& b) noexcept { a.swap(b); }

// Acquire a record from the default domain and return an owning handle.
// The only function in the public interface allowed to allocate, and the only
// one that does -- [saferecl.hp.make]/3, "Throws: May throw bad_alloc".
[[nodiscard]] inline hazard_pointer make_hazard_pointer() {
  return hazard_pointer(detail::hazptr_default_domain().acquire_rec());
}

// --- hazard_pointer_obj_base implementation ---------------------------------

template <typename T, typename D>
inline void hazard_pointer_obj_base<T, D>::retire(D d) noexcept {
  // noexcept per std API, and now actually honoured on the allocation side:
  // retire_impl() splices this object onto an intrusive list, so there is no
  // vector to grow and no way for OOM to reach std::terminate(). The only
  // operation left that can throw is std::mutex::lock (system_error), which is
  // not a memory-pressure failure; removing it needs the lock-free slot claim
  // tracked as R3.

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
  // [32.11.3.3]/6 precondition: x is not retired. The next == this sentinel
  // backs it in every build, not only under contracts -- unlike the bool it
  // replaces, which made sizeof(hazard_pointer_obj_base) depend on whether the
  // translation unit was compiled with contracts enabled.
#ifndef __cpp_contracts
  assert(not_retired() && "hazard_pointer_obj_base::retire: object is already retired");
#endif

  deleter = std::move(d);

  // Type erasure: the domain holds HazptrObj*, and only this instantiation knows
  // T and D. The downcast to hazard_pointer_obj_base is a real derived-to-base
  // cast in reverse, so it works at any offset -- no layout assumption. (The
  // previous version asserted the deleter sat at offset 0, which was never
  // needed and stops holding now that the private base carries members.)
  reclaim = [](detail::HazptrObj* p) {
    auto* base = static_cast<hazard_pointer_obj_base*>(p);
    T* self = static_cast<T*>(base);
    D deleter_copy = std::move(base->deleter);
    deleter_copy(self);
  };

  // Pass the HazptrObj subobject, not the T*: that is the address
  // reset_protection() publishes and synchronize() compares against.
  detail::hazptr_default_domain().retire_impl(this);
}

// --- hazard_pointer implementation ------------------------------------------

inline hazard_pointer::hazard_pointer(detail::HazptrRec* rec) noexcept : rec_(rec) {}

// Two relaxed-cost stores and no lock, which is what P2530R3 1.5 item 3 asks
// of this destructor: it calls it out by name as inlined and latency-critical.
inline hazard_pointer::~hazard_pointer() {
  if (rec_) {
    reset_protection();
    detail::hazptr_default_domain().release_rec(rec_);
  }
}

inline hazard_pointer::hazard_pointer(hazard_pointer&& other) noexcept : rec_(other.rec_) { other.rec_ = nullptr; }

inline hazard_pointer& hazard_pointer::operator=(hazard_pointer&& other) noexcept {
  if (this == &other)
    return *this;
  if (!empty()) {
    reset_protection();
    detail::hazptr_default_domain().release_rec(rec_);
  }
  rec_ = other.rec_;
  other.rec_ = nullptr;
  return *this;
}

inline bool hazard_pointer::empty() const noexcept {
  return rec_ == nullptr; // empty = owns no hazard pointer; unassociated (hazard holds nullptr) is not empty
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
  else if (rec_) {
    // Publish the HazptrObj subobject, not the T*. That is what synchronize()
    // compares against, and the two differ whenever the base is not at offset 0
    // -- e.g. struct T : Other, hazard_pointer_obj_base<T>, which
    // [saferecl.hp.general] p2 permits. Folly normalises the same way, in
    // hazptr_holder::reset_protection. The offset is a compile-time constant,
    // so the reader path pays an add at most.
    const detail::HazptrObj* const obj = ptr; // derived-to-base; private, reachable via friend
    rec_->hazard.store(const_cast<detail::HazptrObj*>(obj), std::memory_order::seq_cst);
  }
}

inline void hazard_pointer::reset_protection(std::nullptr_t) noexcept {
  if (rec_) {
    rec_->hazard.store(nullptr, std::memory_order::release);
  }
}

inline void hazard_pointer::swap(hazard_pointer& other) noexcept { std::swap(rec_, other.rec_); }

// --- HazardDomain implementation --------------------------------------------
namespace detail {
// No pre-allocated pool: records are created on demand and never destroyed
// until the domain is, so a process that never uses hazard pointers pays
// nothing and the constructor cannot fail.
inline HazardDomain::HazardDomain() = default;

inline HazardDomain::~HazardDomain() {
  // Called during static-storage destruction (program exit). Thread-local storage is destroyed
  // before static storage, so tl_node_ is already gone -- synchronize() cannot be called.
  // All threads have exited, so no hazard pointers are held; the protected-set check is
  // unnecessary and every orphan object can be reclaimed unconditionally.
  for (HazptrObj* obj = orphan_list_.head; obj != nullptr;) {
    HazptrObj* const next = obj->next; // read before reclaim(): it frees obj
    obj->reclaim(obj);
    obj = next;
  }
  orphan_list_.clear();

  // Records outlive every hazard_pointer by construction -- release_rec() only
  // clears a flag -- so they are freed here, once all threads are gone.
  for (const HazptrRec* rec = recs_head_.load(std::memory_order::relaxed); rec != nullptr;) {
    const HazptrRec* const next = rec->next;
    delete rec;
    rec = next;
  }
}

inline HazptrRec* HazardDomain::acquire_rec() {
  // Fast path: claim an existing record with a CAS. No lock, so concurrent
  // make_hazard_pointer() calls only contend when they race for the same
  // record. The old code serialised every acquisition on one mutex and scanned
  // a bitmap under it -- review point 3.
  for (HazptrRec* rec = recs_head_.load(std::memory_order::acquire); rec != nullptr; rec = rec->next) {
    bool expected = false;
    if (rec->active.compare_exchange_strong(expected, true, std::memory_order::acquire, std::memory_order::relaxed)) {
      ++active_count_;
      return rec;
    }
  }

  // Every record is taken -- append one. Serialised, but this is the rare path:
  // it runs at most once per concurrently-live hazard_pointer, ever.
  auto* const rec = new HazptrRec(); // throws std::bad_alloc on OOM
  rec->active.store(true, std::memory_order::relaxed);
  {
    const std::lock_guard _(rec_alloc_mutex_);
    rec->next = recs_head_.load(std::memory_order::relaxed);
    // Release, paired with the acquire loads above and in synchronize(). A scan
    // that does not observe this store cannot observe the hazard store that
    // follows it either, and the seq_cst fence in synchronize() already forces
    // the scan to observe any hazard whose reader went on to validate. So a
    // record published after a scan started is one whose reader has not yet
    // committed to protecting anything.
    recs_head_.store(rec, std::memory_order::release);
  }
  ++active_count_;
  ++rec_count_;
  return rec;
}

inline void HazardDomain::release_rec(HazptrRec* rec) noexcept {
  rec->hazard.store(nullptr, std::memory_order::release); // clear the hazard before offering the record for reuse
  --active_count_;
  // Release so that a thread which later claims this record via the acquiring
  // CAS sees the cleared hazard.
  rec->active.store(false, std::memory_order::release);
}

inline std::size_t HazardDomain::active_slots() const noexcept {
  return active_count_.load(std::memory_order::relaxed);
}

inline std::size_t HazardDomain::retire_list_size() const noexcept { return tl_node_.list.size; }

inline void HazardDomain::retire_impl(HazptrObj* obj) noexcept {
  // Lazy registration: on the first retire_impl() call per thread, insert tl_node_ into
  // the global retire_lists_ linked list. tl_node_.registered is safe to check without a
  // lock because only the owning thread ever writes it (and only once, here).
  ensure_node_registered();

  // Splice under list_mutex so synchronize() cannot observe a half-linked list.
  // This is the operation that used to be a vector push_back, i.e. the reason
  // retire() could turn OOM into terminate().
  const std::size_t sz = [&] {
    const std::lock_guard _(tl_node_.list_mutex);
    tl_node_.list.push(obj);
    return tl_node_.list.size;
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

// Splice a collected-but-unscanned list onto the calling thread's own list,
// leaving it empty. Used both for survivors and for the bail-out path when the
// scan cannot be sized; in either case the objects stay retired and reachable,
// so a later synchronize() reclaims them.
//
// "local" means the calling thread's, which is deliberately not the thread the
// objects were retired on -- that one may have exited since the collect.
inline void splice_to_local_list(RetireList& list) noexcept {
  if (list.empty())
    return;
  const std::lock_guard _(tl_node_.list_mutex);
  tl_node_.list.splice(list);
}

inline void HazardDomain::synchronize() noexcept {
  // Ensure this thread is registered so survivors have a valid home to return to.
  // Mirrors the registration in retire_impl(); safe to call from either path.
  ensure_node_registered();

  // Step 1: collect -- detach every thread's retire list, then the orphan list,
  // and concatenate them. Done BEFORE snapshot so that any reader publishing
  // a hazard concurrently with the collect step sees the object still in src (and
  // thus retries) OR has the hazard visible in the snapshot below.
  //
  // Every step here is a pointer splice: the collect phase no longer allocates.
  RetireList pending;
  {
    const std::lock_guard _(retire_lists_mutex_);
    for (RetireListNode* n = retire_lists_head_; n; n = n->next) {
      // The splice touches only the local `pending`, so it does not belong
      // inside list_mutex. Narrowing matters here specifically: list_mutex is
      // the lock retire_impl() takes on the owning thread's hot path, and this
      // loop already holds retire_lists_mutex_ for its whole duration.
      RetireList taken = [&] {
        const std::lock_guard _l(n->list_mutex);
        return n->list.take();
      }();
      pending.splice(taken);
    }
  }
  {
    // Same shape, for symmetry rather than for measurable gain: orphan_mutex_
    // is taken only on thread exit and by synchronize() itself, so it is
    // essentially uncontended.
    RetireList taken = [&] {
      const std::lock_guard _(orphan_mutex_);
      return orphan_list_.take();
    }();
    pending.splice(taken);
  }
  if (pending.empty())
    return;

  // Step 2: size the protected-set buffer. The record list needs no snapshot of
  // its own any more -- records are never unlinked, so the scan below walks it
  // directly, with no lock and no intermediate array.
  //
  // The buffer belongs to this thread and is reused, so it only has to grow
  // when the record pool does: after the first scan, a steady-state reclamation
  // allocates nothing. Growth still has to be able to fail, because a thread
  // that only retires never calls make_hazard_pointer() and so can first reach
  // this point inside a noexcept function.
  //
  // On failure the scan falls back to a linear membership test instead of
  // giving up: reclamation always completes, it is just slower. Each hazard is
  // then re-loaded once per candidate rather than once in total, which is
  // sound -- every load still happens after the fence below, and correctness
  // needs each load to be ordered after it, not to be part of one instant.
  std::vector<void*>& snapshot = tl_node_.scan_buf;
  snapshot.clear();
  bool have_buffer = true;
  try {
    snapshot.reserve(rec_count_.load(std::memory_order::relaxed));
  } catch (...) {
    have_buffer = false;
  }
  // Reclaim-side fence -- MANDATORY, not an optimization barrier.
  //
  // The acquire loads below do not join the seq_cst total order that the reader
  // side relies on, and the collect step's lock chain only orders a *writer's*
  // retirement against the collect: it adds no edge between an independent
  // reader's hazard store and this scan.  Without this fence a reader can
  // re-validate src, still see O (so it keeps dereferencing O), while this scan
  // reads a stale empty slot and frees O.  [saferecl.hp.general] p6 requires the
  // end of the protection epoch to strongly happen before the reclamation, which
  // an acquire-only scan does not provide.
  //
  // Upgrading the loads below to seq_cst instead of fencing does NOT fix it: the
  // removal store on src is user code, and P2530R3 does not require it to be
  // seq_cst, so the StoreLoad reordering survives.  tools/litmus/ locks all
  // three verdicts (Sometimes / Sometimes / Never) into CI.
  //
  // Same shape as Folly's do_reclamation(), which issues a seq_cst
  // asymmetric_thread_fence_heavy immediately before load_hazptr_vals().  Only
  // the reclaimer pays; the reader path is untouched.
  //
  // Found in review by Thomas Rodgers, confirmed with Maged Michael, and
  // observed on POWER9/POWER10 hardware (1.4M of 959M runs):
  // https://gcc.gnu.org/pipermail/libstdc++/2026-July/067282.html
  std::atomic_thread_fence(std::memory_order::seq_cst);

  HazptrRec* const recs = recs_head_.load(std::memory_order::acquire);

  if (have_buffer) {
    for (const HazptrRec* rec = recs; rec != nullptr; rec = rec->next) {
      // snapshot is a vector<void*>, so a const void* here would not survive
      // the push_back below.
      // NOLINTNEXTLINE(misc-const-correctness)
      if (void* const ptr = rec->hazard.load(std::memory_order::acquire))
        snapshot.push_back(ptr); // within the capacity reserved above
    }
    std::ranges::sort(snapshot);
  }

  // Step 3: reclaim -- reclaim every object not in the protected set.
  // No lock held here; deleters can safely call retire() or even synchronize().
  //
  // Both sides of the comparison are HazptrObj subobject addresses: retire()
  // passes one, and reset_protection() publishes one.
  const auto protected_ = [&](const HazptrObj* obj) noexcept {
    if (have_buffer)
      return std::ranges::binary_search(snapshot, static_cast<const void*>(obj));
    for (const HazptrRec* rec = recs; rec != nullptr; rec = rec->next) {
      if (rec->hazard.load(std::memory_order::acquire) == static_cast<const void*>(obj))
        return true;
    }
    return false;
  };

  RetireList survivors;
  for (HazptrObj* obj = pending.head; obj != nullptr;) {
    HazptrObj* const next = obj->next; // read before the object is spliced or freed
    if (protected_(obj))
      survivors.push(obj);
    else
      obj->reclaim(obj);
    obj = next;
  }

  // Step 4: put survivors back into the calling thread's own list.
  // Survivors cannot be returned to their original threads because those threads may
  // have exited between the collect and here. The calling thread's list is guaranteed
  // alive for the duration of this call.
  splice_to_local_list(survivors);
}

inline void HazardDomain::ensure_node_registered() {
  if (tl_node_.registered)
    return;
  {
    const std::lock_guard _(retire_lists_mutex_);
    tl_node_.next = retire_lists_head_;
    retire_lists_head_ = &tl_node_;
    ++retire_list_node_count_;
  }
  tl_node_.registered = true; // set only after successful registration
}

inline RetireListNode::~RetireListNode() {
  if (!registered)
    return; // ensure_node_registered never ran -- nothing to do

  HazardDomain& domain = hazptr_default_domain();

  // Reclaim as much as possible before this thread's retire list goes away.
  // synchronize() puts survivors back into tl_node_.list, which is a member of
  // *this and therefore still alive throughout this destructor body.
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
  if (!list.empty()) {
    const std::lock_guard _(domain.orphan_mutex_);
    domain.orphan_list_.splice(list);
  }
}
} // namespace detail

} // namespace proto
