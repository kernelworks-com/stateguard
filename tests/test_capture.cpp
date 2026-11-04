#include "stateguard/stateguard.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace stateguard;

constexpr std::size_t kThreads = 8;
constexpr std::size_t kEventsPerThread = 1000;
constexpr std::size_t kCapacity = 3000;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

Event allocation(std::size_t thread, std::size_t index) {
  return AllocateEvent{AllocationId("t" + std::to_string(thread) + "-a" + std::to_string(index)),
                       1, 64, "cpu"};
}

// Records kEventsPerThread events from each of kThreads threads at once and
// returns the number of record() calls that reported a kept event.
std::size_t record_concurrently(LockedTraceRecorder& recorder) {
  std::atomic<std::size_t> kept{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      while (!go.load()) std::this_thread::yield();
      for (std::size_t i = 0; i < kEventsPerThread; ++i) {
        if (recorder.record(allocation(t, i))) kept.fetch_add(1);
      }
    });
  }
  go.store(true);
  for (auto& thread : threads) thread.join();
  return kept.load();
}

std::size_t gap_count(const Trace& trace) {
  std::size_t gaps = 0;
  for (const auto& event : trace.events) {
    if (std::holds_alternative<CoverageGapEvent>(event)) ++gaps;
  }
  return gaps;
}

// The finished trace has one gap, ends with the end marker, round-trips
// through the serializer, and the checker cannot call it clean.
void require_overflowed_trace(const Trace& trace) {
  require(gap_count(trace) == 1, "overflowed capture must record exactly one gap");
  require(std::holds_alternative<EndTraceEvent>(trace.events.back()),
          "trace must end with the end marker");
  const auto text = serialize_trace(trace);
  const auto parsed = parse_trace(text);
  require(parsed.events == trace.events, "serialized trace must parse back unchanged");
  require(check(parsed).status == CheckStatus::Inconclusive,
          "overflowed capture must be inconclusive");
}

void test_concurrent_overflow_without_clock() {
  LockedTraceRecorder recorder(kCapacity);
  const auto kept = record_concurrently(recorder);
  const auto stats = recorder.stats();
  require(stats.recorded == kCapacity, "recorder must fill its capacity");
  require(kept == stats.recorded, "kept results must match recorded count");
  require(stats.recorded + stats.dropped == kThreads * kEventsPerThread,
          "recorded + dropped must equal every record() call");
  const auto trace = recorder.finish();
  require(recorder.stats().finished, "stats must report finished");
  require_overflowed_trace(trace);
}

void test_concurrent_overflow_with_clock() {
  LockedTraceRecorder recorder(kCapacity);
  // A manual monotonic clock that advances on every reading, so every kept
  // event is preceded by its own timestamp.
  std::atomic<std::uint64_t> ticks{0};
  recorder.set_clock(TraceClock{"manual", [&ticks] { return ticks.fetch_add(1) + 1; }});
  const auto kept = record_concurrently(recorder);
  const auto stats = recorder.stats();
  require(stats.recorded == kCapacity, "recorder must fill its capacity");
  require(stats.timestamps == kCapacity / 2, "every kept event must be stamped");
  require(kept == stats.recorded - stats.timestamps, "kept results must match events");
  require(stats.recorded - stats.timestamps + stats.dropped == kThreads * kEventsPerThread,
          "kept + dropped events must equal every record() call");
  const auto trace = recorder.finish();
  std::uint64_t last = 0;
  bool after_gap = false;
  for (std::size_t i = 0; i < trace.events.size(); ++i) {
    const auto& event = trace.events[i];
    if (std::holds_alternative<CoverageGapEvent>(event)) after_gap = true;
    if (const auto* stamp = std::get_if<TimestampEvent>(&event)) {
      require(stamp->ns >= last, "timestamps must never decrease in the trace");
      last = stamp->ns;
      // Before the gap a stamp is always paired with the event it precedes.
      if (!after_gap) {
        require(i + 1 < trace.events.size() &&
                    std::holds_alternative<AllocateEvent>(trace.events[i + 1]),
                "a timestamp must be kept together with its event");
      }
    }
  }
  require_overflowed_trace(trace);
}

void test_locked_pair_dropped_together() {
  LockedTraceRecorder recorder(3);
  std::uint64_t now = 0;
  recorder.set_clock(TraceClock{"manual", [&now] { return ++now; }});
  require(recorder.record(allocation(0, 0)), "first stamped event must fit");
  // One slot is left, but the stamp and the event need two.
  require(!recorder.record(allocation(0, 1)), "pair must be dropped when only one slot is left");
  const auto stats = recorder.stats();
  require(stats.recorded == 2 && stats.timestamps == 1 && stats.dropped == 1,
          "a dropped pair must leave no lone timestamp");
  const auto trace = recorder.finish();
  require(trace.events.size() == 5, "trace must be time, event, gap, end time, end");
  require(std::holds_alternative<TimestampEvent>(trace.events[0]) &&
              std::holds_alternative<AllocateEvent>(trace.events[1]) &&
              std::holds_alternative<CoverageGapEvent>(trace.events[2]) &&
              std::holds_alternative<TimestampEvent>(trace.events[3]) &&
              std::holds_alternative<EndTraceEvent>(trace.events[4]),
          "unexpected locked trace layout");
}

void test_locked_forwards_exceptions() {
  bool rejected = false;
  try {
    LockedTraceRecorder zero(0);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "zero capacity must be rejected");
  LockedTraceRecorder recorder(4);
  rejected = false;
  try {
    (void)recorder.record(EndTraceEvent{true});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "end marker must be rejected by record()");
  require(recorder.record(allocation(0, 0)), "event must be kept");
  rejected = false;
  try {
    recorder.set_clock(steady_trace_clock());
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "clock after recording must be rejected");
  (void)recorder.finish();
  rejected = false;
  try {
    (void)recorder.finish();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "second finish must be rejected");
  rejected = false;
  try {
    (void)recorder.record(allocation(0, 1));
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "record after finish must be rejected");
}

}  // namespace

int main() {
  try {
    test_concurrent_overflow_without_clock();
    test_concurrent_overflow_with_clock();
    test_locked_pair_dropped_together();
    test_locked_forwards_exceptions();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "capture tests passed\n";
  return 0;
}
