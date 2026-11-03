#include "cancellation.hpp"
#include "stateguard/stateguard.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace stateguard;
using namespace stateguard::harness;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Exception, typename F>
void require_throws(F&& f, const std::string& message) {
  try {
    f();
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-12 + 1e-9 * std::fabs(b); }

bool mentions(const TimelineReport& report, const std::string& text) {
  return std::any_of(report.incomplete.begin(), report.incomplete.end(),
                     [&text](const std::string& r) { return r.find(text) != std::string::npos; });
}

// A clock that moves only when the test advances it.
struct ManualClock {
  std::shared_ptr<std::uint64_t> now = std::make_shared<std::uint64_t>(1000);
  TraceClock clock() const {
    auto value = now;
    return TraceClock{"manual", [value] { return *value; }};
  }
  std::function<void()> advance(std::uint64_t ns) const {
    auto value = now;
    return [value, ns] { *value += ns; };
  }
};

SemanticDescriptor descriptor() {
  return {NamespaceId("demo"), "model-r1", "cpu-cache", "layer-0", "prefix-1",
          0, 16, "fp16", "row-major", "absolute"};
}

void test_round_trip_of_new_events() {
  Trace trace;
  trace.events = {TimestampEvent{"steady", 42}, CancelSignalEvent{RequestId("r-1")},
                  FinishRequestEvent{RequestId("r-2")}, CancelRequestEvent{RequestId("r-1")},
                  EndTraceEvent{true}};
  const auto text = serialize_trace(trace);
  require(text.find("time|steady|42\ncancel_signal|r-1\nfinish|r-2\n") != std::string::npos,
          "new events use their documented lines");
  require(parse_trace(text).events == trace.events, "new events round trip");
  for (const char* bad : {"STATEGUARD_TRACE|1\ntime|steady\n",
                          "STATEGUARD_TRACE|1\ntime|steady|-1\n",
                          "STATEGUARD_TRACE|1\ntime||5\n",
                          "STATEGUARD_TRACE|1\nfinish|r|x\n",
                          "STATEGUARD_TRACE|1\ncancel_signal|\n"}) {
    require_throws<TraceParseError>([bad] { parse_trace(bad); },
                                    std::string("rejects ") + bad);
  }
  Trace invalid;
  invalid.events = {TimestampEvent{"a:b", 1}};
  require_throws<std::invalid_argument>([&] { serialize_trace(invalid); },
                                        "rejects a reserved character in a clock");
}

Trace consumed_then(Event ending) {
  const AllocationRef buffer{AllocationId("buffer-1"), 1};
  const auto desc = descriptor();
  Trace trace;
  trace.events = {AllocateEvent{buffer.allocation, 1, 4096, "cpu"},
                  BindStateEvent{buffer, StateId("state-1"), desc},
                  SubmitEvent{OperationId("load-1"), RequestId("loader"),
                              {Lease{buffer, AccessKind::Write}}, {}},
                  CompleteAccessEvent{OperationId("load-1")},
                  PublishEvent{StateId("state-1"), buffer, OperationId("load-1")},
                  ConsumeEvent{RequestId("request-1"), StateId("state-1"), buffer, desc},
                  std::move(ending),
                  RetireEvent{buffer},
                  ReclaimEvent{buffer},
                  EndTraceEvent{true}};
  return trace;
}

void test_checker_semantics_of_new_events() {
  require(check(consumed_then(FinishRequestEvent{RequestId("request-1")})).clean(),
          "finish releases the consumer role");
  const auto signalled = check(consumed_then(CancelSignalEvent{RequestId("request-1")}));
  require(signalled.status == CheckStatus::Violation &&
              signalled.diagnostics.front().category == DiagnosticCategory::PrematureReclaim,
          "a signal alone does not release the consumer");
  auto timed = consumed_then(FinishRequestEvent{RequestId("request-1")});
  for (std::size_t i = timed.events.size() - 1; i > 0; --i) {
    timed.events.insert(timed.events.begin() + static_cast<std::ptrdiff_t>(i),
                        TimestampEvent{"steady", i});
  }
  require(check(timed).clean(), "timestamps do not change lifetimes");
  Trace bad;
  bad.events = {TimestampEvent{"", 1}, FinishRequestEvent{RequestId("")},
                CancelSignalEvent{RequestId("a|b")}, EndTraceEvent{true}};
  const auto report = check(bad);
  require(report.status == CheckStatus::Violation && report.diagnostics.size() == 3,
          "malformed new events are invalid transitions");
}

void test_recorder_clock() {
  ManualClock manual;
  TraceRecorder recorder(5);
  recorder.set_clock(manual.clock());
  require(recorder.record(CancelSignalEvent{RequestId("r")}), "first event fits");
  require(recorder.record(CancelRequestEvent{RequestId("r")}), "same reading adds no stamp");
  manual.advance(5)();
  // The last two slots take the stamp and the event together.
  require(recorder.record(FinishRequestEvent{RequestId("q")}), "stamp and event fit");
  require(!recorder.record(FinishRequestEvent{RequestId("q")}), "full buffer drops");
  const auto stats = recorder.stats();
  require(stats.recorded == 5 && stats.timestamps == 2 && stats.dropped == 1,
          "timestamps count as recorded events");
  const auto trace = recorder.finish();
  require(trace.events.size() == 7 && std::holds_alternative<CoverageGapEvent>(trace.events[5]),
          "the end reading equals the last one, so it adds no stamp");
  require(std::get<TimestampEvent>(trace.events[0]) == TimestampEvent{"manual", 1000} &&
              std::get<TimestampEvent>(trace.events[3]) == TimestampEvent{"manual", 1005},
          "each reading is stamped once, before its event");

  TraceRecorder pair(3);
  pair.set_clock(manual.clock());
  pair.record(CancelSignalEvent{RequestId("r")});
  manual.advance(1)();
  require(!pair.record(CancelRequestEvent{RequestId("r")}),
          "an event whose stamp does not fit is dropped with it");
  require(pair.stats().recorded == 2, "no stamp is left without its event");
  const auto ended = pair.finish();
  require(ended.events.size() == 5 &&
              std::get<TimestampEvent>(ended.events[3]) == TimestampEvent{"manual", 1006},
          "finish stamps the end of capture after the gap");

  TraceRecorder late(4);
  late.record(CancelSignalEvent{RequestId("r")});
  require_throws<std::logic_error>([&] { late.set_clock(manual.clock()); },
                                   "clock cannot be set after recording");
  TraceRecorder invalid(4);
  require_throws<std::invalid_argument>([&] { invalid.set_clock(TraceClock{"a|b", [] {
                                          return std::uint64_t{1}; }}); },
                                        "clock name must be an identifier");
  require_throws<std::invalid_argument>([&] { invalid.set_clock(TraceClock{"x", {}}); },
                                        "clock needs a function");
}

constexpr std::uint64_t scheduler_ns = 300;
constexpr std::uint64_t copy_ns = 5000;

CancellationOptions manual_options(const ManualClock& manual) {
  CancellationOptions options;
  options.clock = manual.clock();
  options.scheduler_delay = manual.advance(scheduler_ns);
  options.copy_delay = manual.advance(copy_ns);
  return options;
}

void test_injected_delay_gives_known_interval() {
  ManualClock manual;
  const auto run = run_cancellation(manual_options(manual));
  require(check(run.trace).clean(), "deferred reclaim is clean");
  const auto report = build_timeline(run.trace);
  require(report.status == TimelineStatus::Complete && report.clock == "manual",
          "timeline is complete");
  require(report.requests.size() == 1, "one cancelled request");
  const auto& r = report.requests.front();
  const auto signal = *r.signal_ns;
  require(*r.ack_ns - signal == scheduler_ns, "signal to acknowledgment is the scheduler delay");
  require(*r.last_access_ns - signal == scheduler_ns + copy_ns,
          "last access is the delayed copy's completion");
  require(*r.eligible_ns - signal == scheduler_ns + copy_ns, "eligible when the copy ends");
  require(*r.reuse_ns - signal == scheduler_ns + copy_ns,
          "cancellation to reuse is the injected interval");
  require(r.cancelled_only.size() == 2, "cache and output are cancelled-only");
  // Retention counts from the acknowledgment: the copy delay, for both buffers.
  require(near(report.cancelled_only_byte_seconds, 2 * 4096.0 * copy_ns / 1e9),
          "byte-seconds integrate both buffers over the copy delay");
  require(report.peak_cancelled_only_bytes == 2 * 4096, "peak counts both buffers");
  for (const auto& a : report.allocations) {
    require(!a.reclaimed_before_eligible && !a.retained_at_end, "no premature or open retention");
  }
}

void test_shared_live_consumer_prevents_attribution() {
  ManualClock manual;
  auto options = manual_options(manual);
  options.shared_live_consumer = true;
  const auto run = run_cancellation(options);
  require(check(run.trace).clean(), "shared run is clean");
  const auto report = build_timeline(run.trace);
  require(report.status == TimelineStatus::Complete, "shared timeline is complete");
  const auto& r = report.requests.front();
  require(r.cancelled_only.size() == 1 && r.cancelled_only.front() == AllocationRef{AllocationId("slot-0"), 2},
          "only the output buffer is attributed");
  require(std::any_of(r.not_attributed.begin(), r.not_attributed.end(),
                      [](const AllocationRef& a) { return a == AllocationRef{AllocationId("slot-1"), 1}; }),
          "the shared cache buffer is not attributed");
  require(near(report.cancelled_only_byte_seconds, 4096.0 * copy_ns / 1e9) &&
              report.peak_cancelled_only_bytes == 4096,
          "shared bytes are not charged to the cancellation");
}

void test_missing_completion_is_incomplete() {
  ManualClock manual;
  auto options = manual_options(manual);
  options.complete_copy = false;
  const auto run = run_cancellation(options);
  require(check(run.trace).status == CheckStatus::Inconclusive, "checker is inconclusive");
  const auto report = build_timeline(run.trace);
  require(report.status == TimelineStatus::Incomplete &&
              mentions(report, "operation copy-1 of request request-1 has no observed access completion"),
          "missing completion makes the timeline incomplete");
  const auto& r = report.requests.front();
  require(!r.last_access_ns && !r.eligible_ns && !r.reuse_ns, "unknown stages stay unset");
  // Both buffers stay retained from the acknowledgment to the stamped end.
  require(near(report.cancelled_only_byte_seconds, 2 * 4096.0 * copy_ns / 1e9),
          "open retention is measured to the end of capture");
  require(std::all_of(report.allocations.begin(), report.allocations.end(),
                      [](const AllocationTimeline& a) {
                        return !a.cancelled_only || a.retained_at_end;
                      }),
          "retention open at the end is marked a lower bound");
}

void test_real_clock_interval_covers_the_delay() {
  CancellationOptions options;
  options.clock = steady_trace_clock();
  const auto delay = std::chrono::milliseconds(2);
  options.copy_delay = [delay] { std::this_thread::sleep_for(delay); };
  const auto report = build_timeline(run_cancellation(options).trace);
  const auto& r = report.requests.front();
  require(report.status == TimelineStatus::Complete && report.clock == "steady",
          "steady-clock timeline is complete");
  require(*r.reuse_ns - *r.signal_ns >= 2000000, "measured interval covers the sleep");
}

// Two cancelled requests share one buffer; a third revives a second buffer.
void test_deduplication_and_revival() {
  const AllocationRef shared{AllocationId("buffer-1"), 1};
  const AllocationRef revived{AllocationId("buffer-2"), 1};
  const auto desc = descriptor();
  const auto at = [](std::uint64_t ns) { return TimestampEvent{"manual", ns}; };
  Trace trace;
  trace.events = {at(0),
                  AllocateEvent{shared.allocation, 1, 1000, "cpu"},
                  AllocateEvent{revived.allocation, 1, 500, "cpu"},
                  SubmitEvent{OperationId("w-1"), RequestId("a"),
                              {Lease{shared, AccessKind::Read}, Lease{revived, AccessKind::Read}}, {}},
                  SubmitEvent{OperationId("w-2"), RequestId("b"), {Lease{shared, AccessKind::Read}}, {}},
                  CompleteAccessEvent{OperationId("w-1")},
                  CompleteAccessEvent{OperationId("w-2")},
                  at(10), CancelRequestEvent{RequestId("a")},
                  at(30), CancelRequestEvent{RequestId("b")},
                  at(70), SubmitEvent{OperationId("w-3"), RequestId("c"), {Lease{revived, AccessKind::Read}}, {}},
                  CompleteAccessEvent{OperationId("w-3")},
                  at(130), RetireEvent{shared}, ReclaimEvent{shared},
                  EndTraceEvent{true}};
  const auto report = build_timeline(trace);
  require(report.status == TimelineStatus::Complete, "synthetic timeline is complete");
  require(report.requests.size() == 2, "two cancelled requests");
  // buffer-1: from b's acknowledgment (30) to reclaim (130), counted once.
  // buffer-2: from a's acknowledgment (10) until c uses it (70).
  require(near(report.cancelled_only_byte_seconds, (1000.0 * 100 + 500.0 * 60) / 1e9),
          "shared bytes are counted once and revival ends retention");
  require(report.peak_cancelled_only_bytes == 1500, "peak deduplicates");
  require(report.requests[0].cancelled_only.size() == 2 &&
              report.requests[1].cancelled_only.size() == 1,
          "each request lists the buffers its cancellation retained");
  const auto& revived_timeline = report.allocations[1];
  require(revived_timeline.cancelled_only_ns == 60 && !revived_timeline.retained_at_end,
          "revived buffer stops counting");
}

void test_incomplete_evidence() {
  const auto timeline_of = [](std::vector<Event> events) {
    Trace trace;
    trace.events = std::move(events);
    return build_timeline(trace);
  };
  const RequestId r("r");
  const auto untimed = timeline_of({CancelSignalEvent{r}, CancelRequestEvent{r}, EndTraceEvent{true}});
  require(mentions(untimed, "no timestamps") && untimed.incomplete.size() == 2,
          "untimed trace is incomplete, reported once");
  require(mentions(timeline_of({TimestampEvent{"a", 1}, CancelSignalEvent{r}, EndTraceEvent{true}}),
                   "no scheduler acknowledgment"),
          "signal without acknowledgment is incomplete");
  require(mentions(timeline_of({TimestampEvent{"a", 5}, CancelRequestEvent{r},
                                TimestampEvent{"a", 4}, EndTraceEvent{true}}),
                   "timestamp decreases"),
          "decreasing time is incomplete");
  require(mentions(timeline_of({TimestampEvent{"a", 5}, TimestampEvent{"b", 6},
                                CancelRequestEvent{r}, EndTraceEvent{true}}),
                   "clock changes"),
          "a second clock is incomplete");
  require(mentions(timeline_of({TimestampEvent{"a", 5}, CancelRequestEvent{r},
                                CoverageGapEvent{"lost"}, EndTraceEvent{true}}),
                   "coverage gap"),
          "a coverage gap is incomplete");
  require(mentions(timeline_of({TimestampEvent{"a", 5}, CancelRequestEvent{r}}), "no end marker"),
          "a truncated trace is incomplete");
  require(timeline_of({TimestampEvent{"a", 5}, CancelRequestEvent{r}, EndTraceEvent{true}})
                  .status == TimelineStatus::Complete,
          "a timed acknowledged cancellation is complete");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests{
      {"round trip of new events", test_round_trip_of_new_events},
      {"checker semantics of new events", test_checker_semantics_of_new_events},
      {"recorder clock", test_recorder_clock},
      {"injected delay gives the known interval", test_injected_delay_gives_known_interval},
      {"shared live consumer prevents attribution", test_shared_live_consumer_prevents_attribution},
      {"missing completion is incomplete", test_missing_completion_is_incomplete},
      {"real clock interval covers the delay", test_real_clock_interval_covers_the_delay},
      {"deduplication and revival", test_deduplication_and_revival},
      {"incomplete evidence", test_incomplete_evidence},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cout << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
