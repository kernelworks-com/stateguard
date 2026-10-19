#include "async_copy.hpp"
#include "stateguard/stateguard.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace stateguard;
using namespace stateguard::harness;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Exception, typename Function>
void require_throws(Function&& function, const std::string& message) {
  try {
    function();
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

std::string describe(const ScenarioRun& run) {
  std::string text;
  for (const auto& action : run.schedule) text += "\n  " + action;
  return text;
}

std::string describe(const CheckReport& report) {
  std::string text = to_string(report.status);
  for (const auto& d : report.diagnostics) {
    text += "\n  " + std::to_string(d.event_index) + ' ' + to_string(d.category) +
            (d.confirmed ? "" : " [unconfirmed]") + ": " + d.message;
  }
  return text;
}

// Index of the first event matching `predicate`, if any.
std::optional<std::size_t> find_event(const Trace& trace,
                                      const std::function<bool(const Event&)>& predicate) {
  for (std::size_t i = 0; i < trace.events.size(); ++i) {
    if (predicate(trace.events[i])) return i;
  }
  return std::nullopt;
}

std::optional<std::size_t> completion_of(const Trace& trace, const char* operation) {
  return find_event(trace, [&](const Event& e) {
    const auto* c = std::get_if<CompleteAccessEvent>(&e);
    return c != nullptr && c->operation.value == operation;
  });
}

std::optional<std::size_t> cancellation_of(const Trace& trace, const char* request) {
  return find_event(trace, [&](const Event& e) {
    const auto* c = std::get_if<CancelRequestEvent>(&e);
    return c != nullptr && c->request.value == request;
  });
}

// The cache buffer that prefix-1 was bound to, and the index of its retire.
std::optional<std::size_t> retirement_of_state(const Trace& trace, const char* state) {
  std::optional<AllocationRef> bound;
  for (std::size_t i = 0; i < trace.events.size(); ++i) {
    if (const auto* b = std::get_if<BindStateEvent>(&trace.events[i]);
        b != nullptr && b->state.value == state) {
      bound = b->allocation;
    }
    if (const auto* r = std::get_if<RetireEvent>(&trace.events[i]);
        r != nullptr && bound && r->allocation == *bound) {
      return i;
    }
  }
  return std::nullopt;
}

const Diagnostic* first_confirmed_finding(const CheckReport& report) {
  for (const auto& d : report.diagnostics) {
    if (d.category != DiagnosticCategory::IncompleteEvidence && d.confirmed) return &d;
  }
  return nullptr;
}

void test_recorder_bounds_capture_and_marks_loss() {
  require_throws<std::invalid_argument>([] { TraceRecorder recorder(0); },
                                        "zero capacity was accepted");
  TraceRecorder recorder(3);
  for (std::size_t i = 0; i < 5; ++i) {
    const bool kept =
        recorder.record(AllocateEvent{AllocationId("buffer-" + std::to_string(i)), 1, 8, "cpu"});
    require(kept == (i < 3), "event " + std::to_string(i) + " kept state is wrong");
  }
  require_throws<std::invalid_argument>([&] { recorder.record(EndTraceEvent{true}); },
                                        "an end marker was recorded directly");
  const auto stats = recorder.stats();
  require(stats.recorded == 3 && stats.dropped == 2 && !stats.finished,
          "overflow counters are wrong: recorded " + std::to_string(stats.recorded) +
              " dropped " + std::to_string(stats.dropped));

  const auto trace = recorder.finish();
  require(trace.events.size() == 5, "finished trace should hold 3 events, a gap, and an end");
  const auto* gap = std::get_if<CoverageGapEvent>(&trace.events[3]);
  require(gap != nullptr && gap->reason == "capture buffer full after 3 events (2 dropped)",
          "overflow gap is missing or misplaced");
  require(std::holds_alternative<EndTraceEvent>(trace.events[4]), "end marker is missing");
  require(check(trace).status == CheckStatus::Inconclusive,
          "a trace with dropped events must be inconclusive");
  require(parse_trace(serialize_trace(trace)).events == trace.events,
          "overflow gap does not round-trip through the trace format");

  require_throws<std::logic_error>([&] { recorder.record(CancelRequestEvent{RequestId("r")}); },
                                   "recording after finish was accepted");
  require_throws<std::logic_error>([&] { recorder.finish(); }, "finish ran twice");

  TraceRecorder exact(2);
  exact.record(AllocateEvent{AllocationId("buffer-1"), 1, 8, "cpu"});
  exact.record(RetireEvent{{AllocationId("buffer-1"), 1}});
  const auto full = exact.finish();
  require(exact.stats().dropped == 0 && full.events.size() == 3 && check(full).clean(),
          "a capture that exactly fills the buffer must not report loss");
}

void test_pool_checks_handles_and_records_only_accepted_calls() {
  TraceRecorder recorder(64);
  BufferPool pool(recorder, 1, 16);
  const auto first = pool.acquire();
  require(first.slot == 0 && first.generation == 1, "first handle is wrong");
  require_throws<std::runtime_error>([&] { pool.acquire(); }, "exhausted pool handed out a slot");
  require_throws<std::logic_error>([&] { pool.reclaim(first); },
                                   "reclaim without retire was accepted");
  pool.retire(first);
  require_throws<std::logic_error>([&] { pool.retire(first); }, "double retire was accepted");
  pool.reclaim(first);
  const auto second = pool.acquire();
  require(second.slot == 0 && second.generation == 2,
          "reuse must keep the slot and advance the generation");
  require_throws<std::logic_error>([&] { pool.bytes(first); }, "stale handle read bytes");
  require_throws<std::logic_error>([&] { pool.retire(first); }, "stale handle was retired");
  require_throws<std::logic_error>([&] { pool.address(first); }, "stale handle resolved");

  const auto trace = recorder.finish();
  const std::vector<Event> expected = {
      AllocateEvent{AllocationId("slot-0"), 1, 16, "cpu"},
      RetireEvent{{AllocationId("slot-0"), 1}},
      ReclaimEvent{{AllocationId("slot-0"), 1}},
      AllocateEvent{AllocationId("slot-0"), 2, 16, "cpu"},
      EndTraceEvent{true},
  };
  require(trace.events == expected, "pool recorded rejected calls:\n" + serialize_trace(trace));
}

void test_engine_completion_contract() {
  for (const bool duplicate : {false, true}) {
    TraceRecorder recorder(64);
    BufferPool pool(recorder, 2, 4);
    CopyEngine engine(recorder, pool, EngineOptions{true, duplicate, {}});
    const auto source = pool.acquire();
    const auto destination = pool.acquire();
    std::fill(pool.bytes(source).begin(), pool.bytes(source).end(), std::byte{7});
    engine.submit(OperationId("copy"), RequestId("r"), source, destination);
    require_throws<std::logic_error>(
        [&] { engine.submit(OperationId("copy"), RequestId("r"), source, destination); },
        "a reused operation identity was accepted");
    require(engine.accessing(source) && engine.accessing(destination),
            "a pending copy must hold both buffers");
    require(pool.bytes(destination)[0] == std::byte{0} && !engine.has_notifications(),
            "a copy ran or notified before completion");

    engine.complete(OperationId("copy"));
    require(pool.bytes(destination)[3] == std::byte{7}, "completion did not copy the data");
    require(!engine.accessing(source) && engine.pending().empty(),
            "a completed copy still holds its buffers");
    std::size_t notifications = 0;
    while (engine.poll()) ++notifications;
    require(notifications == (duplicate ? 2u : 1u), "wrong number of notifications");
    require_throws<std::logic_error>([&] { engine.complete(OperationId("copy")); },
                                     "a copy completed twice");

    const auto trace = recorder.finish();
    require(std::holds_alternative<CompleteAccessEvent>(trace.events[3]),
            "completion must be recorded once, after the submission");
    require(std::count_if(trace.events.begin(), trace.events.end(), [](const Event& e) {
              return std::holds_alternative<CompleteAccessEvent>(e);
            }) == 1,
            "duplicate notifications must not duplicate the completion event");
  }

  TraceRecorder recorder(64);
  BufferPool pool(recorder, 2, 4);
  CopyEngine engine(recorder, pool, EngineOptions{false, false, {}});
  const auto source = pool.acquire();
  const auto destination = pool.acquire();
  engine.submit(OperationId("copy"), RequestId("r"), source, destination);
  engine.complete(OperationId("copy"));
  require(engine.poll().has_value(), "an engine without completion events still notifies");
  const auto trace = recorder.finish();
  require(std::holds_alternative<CoverageGapEvent>(trace.events.front()),
          "an engine without completion events must declare a coverage gap first");
  require(!completion_of(trace, "copy"), "an engine without completion events recorded one");
  require(check(trace).status == CheckStatus::Inconclusive,
          "missing completion evidence must be inconclusive");
}

void test_safe_policy_is_clean_in_every_schedule() {
  std::size_t completion_after_cancel = 0;
  std::size_t eviction_during_copy = 0;
  std::size_t staging_released_during_load = 0;
  std::size_t load_outlived_its_buffer = 0;
  std::size_t slot_reuse = 0;
  const auto runs = explore_scenario(ScenarioOptions{}, [&](const ScenarioRun& run) {
    const auto report = check(run.trace);
    require(report.clean(), "safe schedule is not clean: " + describe(report) + describe(run));
    require(run.hazards.empty(), "safe schedule reclaimed unsafely:" + describe(run));
    require(run.delivered_intact, "safe schedule corrupted request-2's bytes:" + describe(run));
    require(run.capture.dropped == 0, "safe schedule overflowed the capture buffer");

    const auto& trace = run.trace;
    if (completion_of(trace, "send-1") > cancellation_of(trace, "request-1")) {
      ++completion_after_cancel;
    }
    if (retirement_of_state(trace, "prefix-1") < completion_of(trace, "send-2")) {
      ++eviction_during_copy;
    }
    const auto staging_retire = find_event(trace, [](const Event& e) {
      const auto* r = std::get_if<RetireEvent>(&e);
      return r != nullptr && r->allocation == AllocationRef{AllocationId("slot-0"), 1};
    });
    if (staging_retire < completion_of(trace, "load-1")) ++staging_released_during_load;
    if (retirement_of_state(trace, "prefix-2") < completion_of(trace, "load-2")) {
      ++load_outlived_its_buffer;
    }
    if (find_event(trace, [](const Event& e) {
          const auto* a = std::get_if<AllocateEvent>(&e);
          return a != nullptr && a->generation > 1;
        })) {
      ++slot_reuse;
    }
  });
  std::cout << "  safe policy: " << runs << " schedules clean; completion after cancel "
            << completion_after_cancel << ", eviction during copy " << eviction_during_copy
            << ", staging released during load " << staging_released_during_load
            << ", load outlived its buffer " << load_outlived_its_buffer << ", slot reuse "
            << slot_reuse << '\n';
  require(runs > 1000, "exploration covered too few schedules");
  // Every schedule reuses a reclaimed slot; the other features depend on the
  // completion order and must vary across schedules.
  require(slot_reuse == runs, "a schedule never reused a slot generation");
  for (const auto count : {completion_after_cancel, eviction_during_copy,
                           staging_released_during_load, load_outlived_its_buffer}) {
    require(count > 0 && count < runs, "a scenario feature was never or always exercised");
  }
}

// The checker sees only events; the harness knows which reclaims were unsafe.
// The two must agree on every schedule.
void require_verdict_matches_harness(const ScenarioRun& run, std::size_t& violations,
                                     std::size_t& corrupted) {
  const auto report = check(run.trace);
  const bool unsafe = !run.hazards.empty();
  require((report.status == CheckStatus::Violation) == unsafe,
          std::string(unsafe ? "unsafe reclaim was not reported: "
                             : "safe schedule was reported: ") +
              describe(report) + describe(run));
  if (!run.delivered_intact) {
    require(unsafe, "bytes were corrupted without an unsafe reclaim:" + describe(run));
    ++corrupted;
  }
  if (!unsafe) {
    require(report.clean(), "schedule without hazards is not clean: " + describe(report));
    return;
  }
  ++violations;
  const auto* finding = first_confirmed_finding(report);
  const auto expected = to_ref(run.hazards.front().buffer);
  require(finding != nullptr && finding->category == DiagnosticCategory::PrematureReclaim &&
              finding->witness ==
                  std::vector<std::string>{"allocation=" + expected.allocation.value,
                                           "generation=" + std::to_string(expected.generation)},
          "first finding does not name the first unsafe reclaim: " + describe(report) +
              describe(run));
  const auto& hazard = run.hazards.front();
  const bool says_lease = finding->message.find("operation lease") != std::string::npos;
  const bool says_consumer = finding->message.find("live consumer") != std::string::npos;
  require(says_lease == hazard.pending_access && says_consumer == hazard.live_consumer,
          "finding gives the wrong reason: " + finding->message + describe(run));
}

void test_induced_bugs_are_reported_exactly_when_unsafe() {
  struct Case {
    const char* name;
    ScenarioOptions options;
    // Immediate reclaim always frees prefix-1 while request-2 still uses it,
    // and in some orders the slot is overwritten before send-2 copies it.
    // Redelivery frees buffers early only in some orders. It can free prefix-1
    // while the cancelled request's send-1 still reads it, but request-2's
    // bytes stay intact in every order.
    bool always_unsafe;
    bool corrupts;
  };
  ScenarioOptions immediate;
  immediate.host.reclaim = ReclaimPolicy::Immediate;
  ScenarioOptions redelivery;
  redelivery.engine.duplicate_notifications = true;
  redelivery.host.idempotent_notifications = false;
  for (const auto& c : {Case{"immediate reclaim", immediate, true, true},
                        Case{"non-idempotent redelivery", redelivery, false, false}}) {
    std::size_t violations = 0;
    std::size_t corrupted = 0;
    const auto runs = explore_scenario(c.options, [&](const ScenarioRun& run) {
      require_verdict_matches_harness(run, violations, corrupted);
    });
    std::cout << "  " << c.name << ": " << runs << " schedules, " << violations
              << " violations, " << corrupted << " with corrupted bytes\n";
    require((corrupted > 0) == c.corrupts,
            std::string(c.name) + " corrupted data in " + std::to_string(corrupted) +
                " schedules");
    if (c.always_unsafe) {
      require(violations == runs, std::string(c.name) + " should be unsafe in every schedule");
    } else {
      require(violations > 0 && violations < runs,
              std::string(c.name) + " should be unsafe only in some schedules");
    }
  }

  // Redelivery alone is harmless when the host ignores repeats.
  ScenarioOptions idempotent;
  idempotent.engine.duplicate_notifications = true;
  explore_scenario(idempotent, [](const ScenarioRun& run) {
    require(check(run.trace).clean() && run.hazards.empty() && run.delivered_intact,
            "idempotent host failed under redelivery:" + describe(run));
  });
}

void test_lost_coverage_is_never_clean() {
  struct Case {
    const char* name;
    ScenarioOptions options;
  };
  std::vector<Case> cases;
  for (const auto policy : {ReclaimPolicy::Deferred, ReclaimPolicy::Immediate}) {
    ScenarioOptions overflow;
    overflow.host.reclaim = policy;
    overflow.capture_capacity = 16;
    ScenarioOptions no_completion;
    no_completion.host.reclaim = policy;
    no_completion.engine.reports_completion = false;
    cases.push_back({"overflow", overflow});
    cases.push_back({"no completion", no_completion});
  }
  for (const auto& c : cases) {
    std::size_t confirmed_before_gap = 0;
    explore_scenario(c.options, [&](const ScenarioRun& run) {
      const auto report = check(run.trace);
      require(report.status != CheckStatus::Clean,
              std::string(c.name) + " produced a clean result:" + describe(run));
      const auto gap = find_event(run.trace, [](const Event& e) {
        return std::holds_alternative<CoverageGapEvent>(e);
      });
      require(gap.has_value(), std::string(c.name) + " trace does not mark its loss");
      if (report.status == CheckStatus::Violation) {
        // Only a finding that precedes the loss may be confirmed.
        const auto* finding = first_confirmed_finding(report);
        require(finding != nullptr && finding->event_index < *gap && !run.hazards.empty(),
                std::string(c.name) + " confirmed a finding after its gap: " + describe(report));
        ++confirmed_before_gap;
      }
    });
    if (c.options.engine.reports_completion &&
        c.options.host.reclaim == ReclaimPolicy::Immediate) {
      require(confirmed_before_gap > 0,
              "an unsafe reclaim captured before overflow should stay a violation");
    } else {
      require(confirmed_before_gap == 0,
              std::string(c.name) + " produced a violation without a captured unsafe reclaim");
    }
  }
}

void test_failed_loads_are_retried_or_reported() {
  // load-1 fails in every schedule. A host that retries must stay clean and
  // deliver intact bytes; a host that publishes the failed load must be
  // reported at that publication in every schedule.
  ScenarioOptions retry;
  retry.engine.failing_operations = {"load-1"};
  ScenarioOptions publish = retry;
  publish.host.publish_failed_loads = true;

  const auto retry_runs = explore_scenario(retry, [](const ScenarioRun& run) {
    const auto report = check(run.trace);
    require(report.clean() && run.delivered_intact && run.hazards.empty() &&
                run.failed_publications == 0,
            "retried load was not clean and intact: " + describe(report) + describe(run));
    require(find_event(run.trace, [](const Event& e) {
              const auto* c = std::get_if<CompleteAccessEvent>(&e);
              return c != nullptr && c->operation.value == "load-1" &&
                     c->outcome == AccessOutcome::Failed;
            }).has_value() &&
                completion_of(run.trace, "load-1-retry").has_value(),
            "trace lacks the failed load or its retry" + describe(run));
  });

  const auto publish_runs = explore_scenario(publish, [](const ScenarioRun& run) {
    const auto report = check(run.trace);
    const auto* first = first_confirmed_finding(report);
    require(report.status == CheckStatus::Violation && first != nullptr &&
                first->category == DiagnosticCategory::FailedWritePublication &&
                std::find(first->witness.begin(), first->witness.end(), "operation=load-1") !=
                    first->witness.end(),
            "publishing the failed load was not the first finding: " + describe(report) +
                describe(run));
    require(run.failed_publications == 1 && !run.delivered_intact && run.hazards.empty(),
            "harness ground truth for the failed publication is wrong" + describe(run));
  });
  std::cout << "  failed load: " << retry_runs << " retry schedules clean, " << publish_runs
            << " publish schedules reported\n";
}

void test_schedule_choices_are_validated_and_replayable() {
  ScenarioOptions immediate;
  immediate.host.reclaim = ReclaimPolicy::Immediate;
  require_throws<std::out_of_range>([&] { run_scenario(immediate, {7}); },
                                    "an out-of-range choice was accepted");
  // Complete load-1 before the host releases its staging buffer: the early
  // reclaim then happens after access completion and is not a hazard there.
  const auto first = run_scenario(immediate, {0, 1});
  const auto again = run_scenario(immediate, {0, 1});
  require(first.trace.events == again.trace.events && first.schedule == again.schedule,
          "a schedule did not replay identically");
  require(first.schedule.at(1) == "complete load-1", "choice 1 should complete load-1");
}

void test_harness_trace_reduces_to_witness() {
  ScenarioOptions immediate;
  immediate.host.reclaim = ReclaimPolicy::Immediate;
  const auto run = run_scenario(immediate);
  require(!run.delivered_intact, "the default immediate schedule should corrupt request-2");
  const auto parsed = parse_trace(serialize_trace(run.trace));
  require(parsed.events == run.trace.events, "harness trace does not round-trip");
  const auto reduced = reduce_trace(parsed);
  require(reduced.status == CheckStatus::Violation &&
              reduced.target.category == DiagnosticCategory::PrematureReclaim,
          "reduction lost the premature reclaim");
  require(reduced.trace.events.size() < run.trace.events.size() / 4,
          "harness witness should be much smaller than the capture");
  const auto report = check(reduced.trace);
  require(report.status == CheckStatus::Violation, "reduced harness trace is not a violation");
}

bool has_witness(const Diagnostic& diagnostic, const std::string& line) {
  return std::find(diagnostic.witness.begin(), diagnostic.witness.end(), line) !=
         diagnostic.witness.end();
}

void test_routed_engine_contract() {
  require_throws<std::invalid_argument>(
      [] {
        TraceRecorder recorder(8);
        BufferPool pool(recorder, 2, 4);
        EngineOptions options;
        options.reports_completion = false;
        options.endpoint = EndpointId("peer-a");
        CopyEngine engine(recorder, pool, options);
      },
      "a routed engine without completion reports was accepted");
  {
    TraceRecorder recorder(8);
    BufferPool pool(recorder, 2, 4);
    CopyEngine engine(recorder, pool);
    require_throws<std::logic_error>([&] { engine.reconnect(); },
                                     "an engine without an endpoint reconnected");
  }

  TraceRecorder recorder(64);
  BufferPool pool(recorder, 4, 4);
  EngineOptions options;
  options.endpoint = EndpointId("peer-a");
  options.redeliver_on_reconnect = true;
  CopyEngine engine(recorder, pool, options);
  const auto a = pool.acquire();
  const auto b = pool.acquire();
  const auto c = pool.acquire();
  const auto d = pool.acquire();
  const auto first = engine.submit(OperationId("copy-1"), RequestId("r"), a, b);
  const auto second = engine.submit(OperationId("copy-2"), RequestId("r"), c, d);
  const TransferRoute route_1_1{EndpointId("peer-a"), 1, "1"};
  const TransferRoute route_1_2{EndpointId("peer-a"), 1, "2"};
  require(first == route_1_1 && second == route_1_2, "routes are not counted per epoch");

  engine.complete(OperationId("copy-1"));
  const auto notified = engine.poll();
  require(notified && notified->operation.value == "copy-1" && notified->route == route_1_1 &&
              notified->outcome == AccessOutcome::Succeeded,
          "notification does not name its route");

  // copy-2 is still running when the endpoint reconnects. Its notification
  // keeps its epoch-1 route, and copy-1's is delivered again.
  engine.reconnect();
  engine.complete(OperationId("copy-2"));
  std::vector<Notification> after;
  while (auto n = engine.poll()) after.push_back(*n);
  require(after.size() == 2 && after[0].route == route_1_1 && after[1].route == route_1_2,
          "reconnect must redeliver epoch-1 notifications and keep late routes");

  engine.reconnect();
  require(!engine.has_notifications(),
          "a late epoch-1 notification must not be redelivered after epoch 2");

  const auto trace = recorder.finish();
  require(std::holds_alternative<ConnectEndpointEvent>(trace.events.front()),
          "a routed engine must record its first epoch before any submission");
  require(!completion_of(trace, "copy-1") && !completion_of(trace, "copy-2"),
          "a routed engine recorded a completion itself");
  require(std::count_if(trace.events.begin(), trace.events.end(), [](const Event& e) {
            return std::holds_alternative<ConnectEndpointEvent>(e);
          }) == 3,
          "each epoch must be recorded once");
}

// Ground truth for the routed scenarios: the harness knows which operation
// each notification came from, which reclaims were unsafe, and which
// operations the host never credited. The checker sees only the events.
struct RoutedTally {
  std::size_t runs = 0;
  std::size_t clean = 0;
  std::size_t misattributed = 0;
  std::size_t unsafe = 0;
  std::size_t corrupted = 0;
  std::size_t lost = 0;
  std::size_t lost_only = 0;  // Lost a completion without misattributing one.
};

void tally(RoutedTally& t, const ScenarioRun& run) {
  ++t.runs;
  if (check(run.trace).clean()) ++t.clean;
  if (run.misattributions > 0) ++t.misattributed;
  if (!run.hazards.empty()) ++t.unsafe;
  if (!run.delivered_intact) ++t.corrupted;
  if (run.unacknowledged > 0) ++t.lost;
  if (run.unacknowledged > 0 && run.misattributions == 0) ++t.lost_only;
}

void print(const char* name, const RoutedTally& t) {
  std::cout << "  " << name << ": " << t.runs << " schedules, " << t.clean << " clean, "
            << t.misattributed << " misattributed, " << t.unsafe << " unsafe reclaim, "
            << t.corrupted << " corrupted, " << t.lost << " lost a completion ("
            << t.lost_only << " without misattribution)\n";
}

void require_correct_host(const ScenarioRun& run, const char* name) {
  const auto report = check(run.trace);
  require(report.clean() && run.hazards.empty() && run.delivered_intact &&
              run.misattributions == 0 && run.unacknowledged == 0,
          std::string(name) + " schedule is not clean and intact: " + describe(report) +
              describe(run));
}

// A host that credits a notification to the wrong operation must be reported
// at that completion. A host that drops a real completion as a repeat leaves
// an operation without an observed completion: the checker cannot see the
// dropped message and must report the trace inconclusive, not clean.
void require_attribution_verdict(const ScenarioRun& run, const char* owner) {
  const auto report = check(run.trace);
  const auto* first = first_confirmed_finding(report);
  if (run.misattributions == 0) {
    require(first == nullptr && run.hazards.empty() && run.delivered_intact,
            "a schedule without misattribution has a finding: " + describe(report) +
                describe(run));
    require(report.clean() == (run.unacknowledged == 0),
            std::string(run.unacknowledged > 0 ? "a lost completion was reported clean: "
                                               : "a correct schedule is not clean: ") +
                describe(report) + describe(run));
    return;
  }
  require(report.status == CheckStatus::Violation && first != nullptr &&
              first->category == DiagnosticCategory::MisattributedCompletion &&
              first->message.find(std::string("operation ") + owner) != std::string::npos,
          "the misattributed completion is not the first finding: " + describe(report) +
              describe(run));
}

void test_reconnect_attribution() {
  for (const bool redeliver : {false, true}) {
    ScenarioOptions route;
    route.kind = ScenarioKind::Reconnect;
    route.engine.redeliver_on_reconnect = redeliver;
    ScenarioOptions tag_only = route;
    tag_only.host.attribution = Attribution::TagOnly;

    RoutedTally correct;
    std::size_t late_old_epoch = 0;
    explore_scenario(route, [&](const ScenarioRun& run) {
      require_correct_host(run, "route-checked reconnect");
      tally(correct, run);
      const auto reconnect = find_event(run.trace, [](const Event& e) {
        const auto* c = std::get_if<ConnectEndpointEvent>(&e);
        return c != nullptr && c->epoch == 2;
      });
      if (completion_of(run.trace, "load-1") > reconnect) ++late_old_epoch;
    });
    require(late_old_epoch > 0 && late_old_epoch < correct.runs,
            "load-1 should complete after the reconnect only in some schedules");

    RoutedTally buggy;
    explore_scenario(tag_only, [&](const ScenarioRun& run) {
      require_attribution_verdict(run, "load-1");
      tally(buggy, run);
      if (run.misattributions > 0) {
        const auto report = check(run.trace);
        const auto* first = first_confirmed_finding(report);
        require(has_witness(*first, "operation=load-2") &&
                    has_witness(*first, "evidence=peer-a:1:1"),
                "the finding does not name load-2 and the epoch-1 evidence" + describe(run));
      }
    });
    const auto label = std::string(redeliver ? "with" : "without") + " redelivery";
    print(("reconnect, route-checked host, " + label).c_str(), correct);
    print(("reconnect, tag-only host, " + label).c_str(), buggy);
    require(buggy.misattributed > 0 && buggy.misattributed < buggy.runs,
            "the tag-only host should misattribute only in some schedules");
    require(buggy.unsafe > 0 && buggy.corrupted > 0,
            "a misattributed completion should cause an unsafe reclaim and corrupt data");
    require(buggy.lost_only > 0 && buggy.clean > 0,
            "the tag-only host should also lose a completion, or stay correct, in some orders");
  }
}

void test_timeout_and_retry() {
  ScenarioOptions wait;
  wait.kind = ScenarioKind::TimeoutRetry;
  ScenarioOptions reclaim = wait;
  reclaim.host.timeout = TimeoutPolicy::Reclaim;
  ScenarioOptions latest = wait;
  latest.host.attribution = Attribution::LatestAttempt;

  RoutedTally waiting;
  std::size_t original_after_publish = 0;
  explore_scenario(wait, [&](const ScenarioRun& run) {
    require_correct_host(run, "waiting timeout");
    tally(waiting, run);
    const auto published = find_event(run.trace, [](const Event& e) {
      return std::holds_alternative<PublishEvent>(e);
    });
    if (completion_of(run.trace, "load-1") > published) ++original_after_publish;
  });
  require(original_after_publish > 0 && original_after_publish < waiting.runs,
          "the original load should outlive the retry's publication only in some schedules");
  print("timeout, host waits for the late completion", waiting);

  RoutedTally reclaiming;
  explore_scenario(reclaim, [&](const ScenarioRun& run) {
    tally(reclaiming, run);
    const auto report = check(run.trace);
    const auto* first = first_confirmed_finding(report);
    require(!run.hazards.empty() && report.status == CheckStatus::Violation &&
                first != nullptr && first->category == DiagnosticCategory::PrematureReclaim &&
                first->message.find("load-1 expired: timeout") != std::string::npos,
            "reclaiming on timeout was not reported as an expired lease: " + describe(report) +
                describe(run));
    const auto expected = to_ref(run.hazards.front().buffer);
    require(has_witness(*first, "allocation=" + expected.allocation.value),
            "the finding does not name the first unsafe reclaim" + describe(run));
  });
  require(reclaiming.unsafe == reclaiming.runs && reclaiming.clean == 0,
          "reclaiming on timeout must be unsafe in every schedule");
  print("timeout, host reclaims on timeout", reclaiming);

  RoutedTally crediting;
  explore_scenario(latest, [&](const ScenarioRun& run) {
    tally(crediting, run);
    const auto report = check(run.trace);
    require_attribution_verdict(run, "load-1");
    if (run.misattributions > 0) {
      require(has_witness(*first_confirmed_finding(report), "operation=load-1-retry"),
              "the finding does not name the retry" + describe(run));
    } else {
      // The retry was credited first, so the host dropped the original's
      // real completion as a repeat.
      require(run.unacknowledged > 0, "the original's completion was not lost" + describe(run));
    }
  });
  require(crediting.clean == 0 && crediting.misattributed > 0 && crediting.lost_only > 0 &&
              crediting.misattributed + crediting.lost_only == crediting.runs,
          "the latest-attempt host should misattribute in some schedules and lose the "
          "original's completion in the rest");
  print("timeout, original completion credited to the retry", crediting);
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"recorder bounds and loss marker", test_recorder_bounds_capture_and_marks_loss},
      {"pool handle checks", test_pool_checks_handles_and_records_only_accepted_calls},
      {"engine completion contract", test_engine_completion_contract},
      {"safe policy in every schedule", test_safe_policy_is_clean_in_every_schedule},
      {"induced bugs match harness hazards", test_induced_bugs_are_reported_exactly_when_unsafe},
      {"lost coverage is never clean", test_lost_coverage_is_never_clean},
      {"failed loads", test_failed_loads_are_retried_or_reported},
      {"schedule choices", test_schedule_choices_are_validated_and_replayable},
      {"harness trace reduction", test_harness_trace_reduces_to_witness},
      {"routed engine contract", test_routed_engine_contract},
      {"reconnect attribution", test_reconnect_attribution},
      {"timeout and retry", test_timeout_and_retry},
  };
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      return 1;
    }
  }
  return 0;
}
