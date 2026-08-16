// Microbenchmarks for review point 3 (Thomas Rodgers): the cost of acquiring
// and releasing a hazard pointer.  P2530R3 sec. 3.1 holds up ~4ns for
// construction/destruction, and until now nothing here had a number.
//
// The same source is compiled twice, against two headers:
//   bench_current   -- ../hazard_ptr.hpp, the intrusive/record-list design
//   bench_baseline  -- baseline/hazard_ptr.hpp, the header as of 1eb5325
//
// 1eb5325 is deliberately the baseline rather than the submitted series: it
// already carries the R1 fence (c3442fd), so the delta measured here isolates
// R2 and R3 instead of being confounded by the fence.  The two headers share a
// public API, so this file needs no #ifdef.
#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <hazard_ptr.hpp>
#include <vector>

namespace {

// Deliberately the same shape as the node in review-issues.md's "Measured cost"
// table -- an int and an atomic link -- so the sizeof reported below can be
// compared against the numbers already recorded there rather than being a
// third, differently-shaped measurement.
struct Node : proto::hazard_pointer_obj_base<Node> {
  int value = 0;
  std::atomic<Node*> next{nullptr};
};

// Reported once as a counter rather than printed: the memory trade is part of
// the same answer, and putting it in the results table keeps it with the
// numbers it has to be weighed against.
void BM_Sizeof(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(sizeof(Node));
  }
  state.counters["sizeof_obj_base"] = static_cast<double>(sizeof(proto::hazard_pointer_obj_base<Node>));
  state.counters["sizeof_Node"] = static_cast<double>(sizeof(Node));
  state.counters["sizeof_handle"] = static_cast<double>(sizeof(proto::hazard_pointer));
}
BENCHMARK(BM_Sizeof);

// --- R3 proper: handle acquire + release ------------------------------------
//
// The headline number.  One iteration is make_hazard_pointer() plus
// ~hazard_pointer(), which is what sec. 3.1's ~4ns refers to.
//
// The thread sweep is the half that matters for the finding: the baseline
// serialises every acquisition and every release on one mutex, so it should
// degrade with thread count, while a CAS-per-record walk should not -- until
// the walk itself gets long, which is the residual cost worth knowing about.
void BM_HandleAcquireRelease(benchmark::State& state) {
  for (auto _ : state) {
    auto hp = proto::make_hazard_pointer();
    benchmark::DoNotOptimize(hp);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HandleAcquireRelease)->ThreadRange(1, 16)->UseRealTime();

// Holding N handles at once before releasing them: this is what forces the
// record pool to N, and on the current design it is also what makes the claim
// walk O(N).  A single-handle benchmark cannot see that cost at all, so a
// green single-thread result on its own would be misleading.
void BM_HandleAcquireReleaseDeep(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  std::vector<proto::hazard_pointer> hps;
  hps.reserve(depth);
  for (auto _ : state) {
    for (std::size_t i = 0; i < depth; ++i)
      hps.push_back(proto::make_hazard_pointer());
    hps.clear();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(depth));
}
BENCHMARK(BM_HandleAcquireReleaseDeep)->Arg(1)->Arg(8)->Arg(64)->Arg(256);

// --- reader hot path --------------------------------------------------------
//
// Not part of R3, but it is the other half of what sec. 3.1 advertises, and the
// R2 match-key change (publishing the HazptrObj subobject rather than the T*)
// lands on exactly this path.  The node is never retired, so no reclamation
// interferes.
void BM_ProtectReset(benchmark::State& state) {
  static Node node;
  static std::atomic<Node*> src{&node};
  auto hp = proto::make_hazard_pointer();
  for (auto _ : state) {
    benchmark::DoNotOptimize(hp.protect(src));
    hp.reset_protection();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProtectReset)->ThreadRange(1, 16)->UseRealTime();

// --- R2: the retire path ----------------------------------------------------
//
// Batched so that the node allocations, which dominate and are identical on
// both headers, sit outside the timed region.  What is timed is retire()
// itself plus whatever auto-synchronize it triggers -- the amortised cost a
// user actually pays, not just the push.
//
// This is where the vector-versus-intrusive comparison lives: the baseline
// pushes onto a std::vector (reallocating, and able to throw inside a noexcept
// function), the current header splices.
//
// The eight live handles are not decoration.  The auto-synchronize threshold is
// `retire list size > 2 * active hazard pointers`, so with none held it is
// `> 0` and every single retire() drags a full synchronize() behind it -- the
// benchmark would then measure the scan, not the retire path.  Eight handles
// put a reclamation every ~16 retires, which is the regime the threshold was
// written for.
void BM_Retire(benchmark::State& state) {
  constexpr int kBatch = 1024;
  constexpr int kLiveHandles = 8;
  std::vector<proto::hazard_pointer> hps;
  hps.reserve(kLiveHandles);
  for (int i = 0; i < kLiveHandles; ++i)
    hps.push_back(proto::make_hazard_pointer());

  std::vector<Node*> nodes;
  nodes.reserve(kBatch);
  for (auto _ : state) {
    state.PauseTiming();
    nodes.clear();
    for (int i = 0; i < kBatch; ++i)
      nodes.push_back(new Node());
    state.ResumeTiming();
    for (Node* n : nodes)
      n->retire();
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_Retire);

// --- the retire push, isolated ----------------------------------------------
//
// BM_Retire above is the amortised number, which is what a user pays but which
// reclamation dominates.  This one isolates the push that R2 is actually about:
// enough handles are held that the auto-synchronize threshold is never crossed
// inside the timed region, so what is measured is a std::vector push_back on
// the baseline against a pointer splice on the current header.
//
// Manual timing rather than PauseTiming/ResumeTiming: a pause/resume pair costs
// around a microsecond, which is three orders of magnitude above the operation
// under test.
void BM_RetirePushOnly(benchmark::State& state) {
  constexpr int kBatch = 256;
  constexpr int kLiveHandles = 512; // 2 * 512 > 256, so no auto-synchronize
  std::vector<proto::hazard_pointer> hps;
  hps.reserve(kLiveHandles);
  for (int i = 0; i < kLiveHandles; ++i)
    hps.push_back(proto::make_hazard_pointer());

  std::vector<Node*> nodes;
  nodes.reserve(kBatch);
  for (auto _ : state) {
    nodes.clear();
    for (int i = 0; i < kBatch; ++i)
      nodes.push_back(new Node());

    const auto t0 = std::chrono::steady_clock::now();
    for (Node* n : nodes)
      n->retire();
    const auto t1 = std::chrono::steady_clock::now();
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());

    // Untimed: drain, so the retire list does not grow across iterations and
    // eventually cross the threshold anyway.
    proto::detail::hazptr_default_domain().synchronize();
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_RetirePushOnly)->UseManualTime();

// --- the scan ---------------------------------------------------------------
//
// synchronize() with `records` hazard pointers live and a fixed batch of
// objects to reclaim, so the parameter sweep isolates the cost of the scan
// itself.  The baseline builds an intermediate array of slot addresses under
// the slot mutex; the current header walks the record list directly and reuses
// a per-thread buffer.
//
// Manual timing for the same reason as above -- with PauseTiming the whole
// sweep sat flat at ~950ns on both headers, which was the pause/resume cost and
// not the scan.
//
// The batch is fixed at 16 and the record counts all satisfy 2 * records >= 16,
// so filling the retire list never trips the auto-synchronize threshold and the
// timed call is the only reclamation in the iteration.
void BM_SynchronizeScan(benchmark::State& state) {
  constexpr int kBatch = 16;
  const auto records = static_cast<std::size_t>(state.range(0));
  std::vector<proto::hazard_pointer> hps;
  hps.reserve(records);
  for (std::size_t i = 0; i < records; ++i)
    hps.push_back(proto::make_hazard_pointer());

  // Warm up: on the current header the first scan on this thread sizes the
  // reusable buffer, and measuring that would measure the one allocation the
  // steady state does not perform.
  (new Node())->retire();
  proto::detail::hazptr_default_domain().synchronize();

  for (auto _ : state) {
    for (int i = 0; i < kBatch; ++i)
      (new Node())->retire();

    const auto t0 = std::chrono::steady_clock::now();
    proto::detail::hazptr_default_domain().synchronize();
    const auto t1 = std::chrono::steady_clock::now();
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
  }
}
BENCHMARK(BM_SynchronizeScan)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->UseManualTime();

} // namespace

BENCHMARK_MAIN();
