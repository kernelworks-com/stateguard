#include "stateguard/stateguard.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace stateguard;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string fixture(const char* name) {
  std::ifstream input(std::string(STATEGUARD_SOURCE_DIR) + "/tests/fixtures/" + name);
  if (!input) throw std::runtime_error(std::string("cannot open fixture ") + name);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string label(const GeneratorConfig& config) {
  return "seed " + std::to_string(config.seed) + " fault " + to_string(config.fault) +
         " allocations " + std::to_string(config.allocations) + " steps " +
         std::to_string(config.steps);
}

std::uint64_t fnv1a(const std::string& text) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// Small, default, and wide histories. The small shape forces heavy reuse of a
// single allocation, request, and state.
std::vector<GeneratorConfig> shapes() {
  GeneratorConfig small;
  small.steps = 24;
  small.allocations = 1;
  small.prefixes = 1;
  small.requests = 1;
  GeneratorConfig wide;
  wide.steps = 160;
  wide.allocations = 8;
  wide.prefixes = 4;
  wide.requests = 16;
  return {small, GeneratorConfig{}, wide};
}

std::vector<Fault> planted_faults() {
  std::vector<Fault> faults;
  for (const auto fault : fault_kinds()) {
    if (fault != Fault::None) faults.push_back(fault);
  }
  return faults;
}

bool same_finding(const Diagnostic& a, const Diagnostic& b) {
  return a.category == b.category && a.message == b.message && a.witness == b.witness;
}

void test_generation_is_deterministic() {
  for (auto config : shapes()) {
    for (const auto fault : fault_kinds()) {
      config.fault = fault;
      config.seed = 11;
      const auto first = serialize_trace(generate_trace(config).trace);
      const auto second = serialize_trace(generate_trace(config).trace);
      require(first == second, "same config produced different traces: " + label(config));
      config.seed = 12;
      require(serialize_trace(generate_trace(config).trace) != first,
              "different seeds produced identical traces: " + label(config));
    }
  }

  // Pinned digests catch platform or standard-library dependence in the
  // generator. A deliberate generator change must update these values.
  for (const auto& [fault, digest] : std::vector<std::pair<Fault, std::uint64_t>>{
           {Fault::None, 13961111325922089766ULL},
           {Fault::PrematureReclaim, 13528758902513200994ULL},
       }) {
    GeneratorConfig config;
    config.seed = 42;
    config.fault = fault;
    const auto actual = fnv1a(serialize_trace(generate_trace(config).trace));
    require(actual == digest, "seed 42 " + to_string(fault) +
                                  " digest changed: " + std::to_string(actual));
  }
}

void test_legal_histories_are_clean() {
  // Count lifecycle situations the clean histories must actually exercise, so
  // that "no false positives" is not satisfied by trivially short traces.
  std::map<std::string, std::size_t> seen;
  for (auto config : shapes()) {
    for (std::uint64_t seed = 0; seed < 300; ++seed) {
      config.seed = seed;
      const auto generated = generate_trace(config);
      require(generated.expected_status == CheckStatus::Clean &&
                  !generated.expected_category && !generated.fault_event,
              "legal history carries a fault expectation: " + label(config));
      const auto report = check(generated.trace);
      if (report.status != CheckStatus::Clean || !report.diagnostics.empty()) {
        std::string detail;
        for (const auto& d : report.diagnostics) {
          detail += "\n  " + std::to_string(d.event_index) + " " + to_string(d.category) +
                    ": " + d.message;
        }
        throw std::runtime_error("legal history was flagged: " + label(config) + detail);
      }
      require(report.capture_complete, "legal history lacks complete capture");

      std::map<std::string, std::set<std::string>> holders;
      std::set<std::string> pending;
      std::set<std::string> cancelled_requests;
      std::map<std::string, std::string> request_of;
      std::set<std::string> published;
      std::set<std::string> retired;
      for (const auto& event : generated.trace.events) {
        if (const auto* e = std::get_if<AllocateEvent>(&event)) {
          if (e->generation >= 2) ++seen["allocation reuse"];
        } else if (const auto* e = std::get_if<BindStateEvent>(&event)) {
          holders[e->state.value].insert(e->allocation.allocation.value + "#" +
                                         std::to_string(e->allocation.generation));
          if (holders[e->state.value].size() >= 2) ++seen["state held by several allocations"];
        } else if (const auto* e = std::get_if<SubmitEvent>(&event)) {
          pending.insert(e->operation.value);
          request_of[e->operation.value] = e->request.value;
          if (e->leases.size() >= 2) ++seen["copy between allocations"];
          if (!e->dependencies.empty()) ++seen["operation dependency"];
        } else if (const auto* e = std::get_if<CompleteAccessEvent>(&event)) {
          pending.erase(e->operation.value);
          if (cancelled_requests.contains(request_of[e->operation.value])) {
            ++seen["completion after cancellation"];
          }
        } else if (const auto* e = std::get_if<PublishEvent>(&event)) {
          const auto key = e->allocation.allocation.value + "#" +
                           std::to_string(e->allocation.generation);
          if (!published.insert(key).second) ++seen["duplicate publication"];
          if (!e->completed_operation) ++seen["publication without proof"];
        } else if (const auto* e = std::get_if<ConsumeEvent>(&event)) {
          (void)e;
          ++seen["consumption"];
        } else if (const auto* e = std::get_if<CancelRequestEvent>(&event)) {
          cancelled_requests.insert(e->request.value);
        } else if (const auto* e = std::get_if<RetireEvent>(&event)) {
          const auto key = e->allocation.allocation.value + "#" +
                           std::to_string(e->allocation.generation);
          if (!retired.insert(key).second) ++seen["duplicate retirement"];
        } else if (std::holds_alternative<ReclaimEvent>(event)) {
          ++seen["reclaim"];
        }
      }
    }
  }
  for (const char* situation :
       {"allocation reuse", "state held by several allocations", "copy between allocations",
        "operation dependency", "completion after cancellation", "duplicate publication",
        "publication without proof", "consumption", "duplicate retirement", "reclaim"}) {
    require(seen[situation] > 0, std::string("legal histories never exercised: ") + situation);
  }
}

void test_planted_faults_are_reported_where_planted() {
  for (auto config : shapes()) {
    for (const auto fault : planted_faults()) {
      config.fault = fault;
      for (std::uint64_t seed = 0; seed < 150; ++seed) {
        config.seed = seed;
        const auto generated = generate_trace(config);
        require(generated.fault_event.has_value() && generated.expected_category.has_value(),
                "fault was not planted: " + label(config));
        const auto report = check(generated.trace);
        require(report.status == generated.expected_status,
                "unexpected status " + to_string(report.status) + ": " + label(config));
        require(!report.diagnostics.empty(), "no diagnostic for planted fault: " + label(config));
        for (const auto& diagnostic : report.diagnostics) {
          // Exactly the planted event is reported: the generator never leaks a
          // second, unintended violation into the surrounding history.
          require(diagnostic.category == *generated.expected_category &&
                      diagnostic.event_index == *generated.fault_event,
                  "unexpected diagnostic " + to_string(diagnostic.category) + " at " +
                      std::to_string(diagnostic.event_index) + " (" + diagnostic.message +
                      "): " + label(config));
        }
      }
    }
  }
}

void test_round_trip_preserves_generated_traces() {
  for (const auto fault : fault_kinds()) {
    GeneratorConfig config;
    config.fault = fault;
    for (std::uint64_t seed = 0; seed < 25; ++seed) {
      config.seed = seed;
      const auto trace = generate_trace(config).trace;
      const auto reparsed = parse_trace(serialize_trace(trace));
      require(reparsed.events == trace.events, "round trip changed events: " + label(config));
      require(check(reparsed).status == check(trace).status,
              "round trip changed the result: " + label(config));
    }
  }
}

void test_configuration_is_validated() {
  for (const auto& mutate : std::vector<void (*)(GeneratorConfig&)>{
           [](GeneratorConfig& c) { c.allocations = 0; },
           [](GeneratorConfig& c) { c.prefixes = 0; },
           [](GeneratorConfig& c) { c.requests = 0; },
           [](GeneratorConfig& c) { c.steps = 1000001; },
           [](GeneratorConfig& c) { c.fault = static_cast<Fault>(99); },
       }) {
    GeneratorConfig config;
    mutate(config);
    bool rejected = false;
    try {
      (void)generate_trace(config);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "invalid generator configuration was accepted");
  }
  GeneratorConfig empty;
  empty.steps = 0;
  require(check(generate_trace(empty).trace).status == CheckStatus::Clean,
          "a zero-step history should still be a clean, ended trace");
  for (const auto fault : planted_faults()) {
    empty.fault = fault;
    const auto generated = generate_trace(empty);
    require(check(generated.trace).status == generated.expected_status,
            "a zero-step history must still plant " + to_string(fault));
  }
}

void require_valid_reduction(const Trace& original, const ReductionResult& result,
                             const std::string& context) {
  const ReductionOracle oracle(original);
  require(oracle.reproduced_by(result.trace), "reduced trace lost the failure: " + context);
  require(result.one_minimal, "reduction did not reach a 1-minimal trace: " + context);
  require(result.trace.events.size() == result.kept_events.size(),
          "kept indices do not match the reduced trace: " + context);
  for (std::size_t i = 0; i < result.kept_events.size(); ++i) {
    require(i == 0 || result.kept_events[i - 1] < result.kept_events[i],
            "reduction reordered events: " + context);
    require(result.kept_events[i] < original.events.size() &&
                original.events[result.kept_events[i]] == result.trace.events[i],
            "reduction rewrote an event: " + context);
  }

  // Independently confirm the oracle's guarantees on the reduced report.
  const auto original_report = check(original);
  const auto reduced_report = check(result.trace);
  require(reduced_report.status == original_report.status,
          "reduction changed the status: " + context);
  for (const auto& diagnostic : reduced_report.diagnostics) {
    bool known = false;
    for (const auto& before : original_report.diagnostics) {
      known = known || same_finding(before, diagnostic);
    }
    require(known, "reduction manufactured a new diagnostic (" + diagnostic.message +
                       "): " + context);
  }

  for (std::size_t i = 0; i < result.trace.events.size(); ++i) {
    auto smaller = result.trace;
    smaller.events.erase(smaller.events.begin() + static_cast<std::ptrdiff_t>(i));
    require(!oracle.reproduced_by(smaller),
            "event " + std::to_string(i) + " was removable: " + context);
  }
}

void test_reduction_preserves_planted_failures() {
  std::size_t before = 0;
  std::size_t after = 0;
  std::size_t largest = 0;
  for (auto config : shapes()) {
    for (const auto fault : planted_faults()) {
      config.fault = fault;
      for (std::uint64_t seed = 0; seed < 10; ++seed) {
        config.seed = seed;
        const auto generated = generate_trace(config);
        const auto result = reduce_trace(generated.trace);
        const auto context = label(config);
        require_valid_reduction(generated.trace, result, context);
        require(result.target.category == *generated.expected_category,
                "reduction targeted the wrong category: " + context);
        require(result.status == generated.expected_status,
                "reduction targeted the wrong status: " + context);

        if (seed < 3) {
          const auto again = reduce_trace(generated.trace);
          require(again.kept_events == result.kept_events && again.checks == result.checks,
                  "reduction is not deterministic: " + context);
        }
        const auto fixed = reduce_trace(result.trace);
        require(fixed.trace.events == result.trace.events,
                "a reduced trace is not a fixed point: " + context);

        before += generated.trace.events.size();
        after += result.trace.events.size();
        largest = std::max(largest, result.trace.events.size());
      }
    }
  }
  // Witnesses keep every obligation the diagnostic names (for example both a
  // live lease and a live consumer) and the dependencies those need.
  require(largest <= 16, "a planted failure reduced to " + std::to_string(largest) + " events");
  std::cout << "  reduced " << before << " generated events to " << after
            << " (largest witness " << largest << " events)\n";
}

void test_reduction_of_fixtures() {
  const auto premature = parse_trace(fixture("premature_reclaim.sgtrace"));
  const auto premature_result = reduce_trace(premature);
  require_valid_reduction(premature, premature_result, "premature_reclaim fixture");
  // The semantic binding is irrelevant to the lease violation; the lease,
  // retirement, and reclaim are not.
  require(premature_result.kept_events == std::vector<std::size_t>({0, 2, 3, 4, 5}),
          "unexpected premature_reclaim witness");
  require(premature_result.target.category == DiagnosticCategory::PrematureReclaim,
          "premature_reclaim fixture targeted the wrong category");

  const auto stale = parse_trace(fixture("stale_generation.sgtrace"));
  const auto stale_result = reduce_trace(stale);
  require_valid_reduction(stale, stale_result, "stale_generation fixture");
  // Generation 2 must stay: it is what makes the generation-1 reference stale.
  // Generation 1's own lifecycle is not needed to show that relation.
  require(stale_result.kept_events == std::vector<std::size_t>({5, 8, 9}),
          "unexpected stale_generation witness");

  for (const char* name : {"incompatible_semantics.sgtrace", "write_shared_state.sgtrace"}) {
    const auto trace = parse_trace(fixture(name));
    require_valid_reduction(trace, reduce_trace(trace), name);
  }
}

void test_oracle_rejects_manufactured_failures() {
  const auto stale = parse_trace(fixture("stale_generation.sgtrace"));
  const ReductionOracle oracle(stale);
  require(oracle.target().category == DiagnosticCategory::StaleGeneration,
          "stale fixture oracle targeted the wrong category");
  require(oracle.reproduced_by(stale), "oracle rejected its own trace");

  // Dropping the reallocation turns the stale reference into a use of a free
  // allocation: still a violation, but not the original failure.
  auto without_reuse = stale;
  without_reuse.events.erase(without_reuse.events.begin() + 5);
  require(check(without_reuse).status == CheckStatus::Violation,
          "test precondition: the mutated trace should still be a violation");
  require(!oracle.reproduced_by(without_reuse),
          "oracle accepted a manufactured invalid state transition");

  // Dropping the first completion makes publication precede completion: a
  // different failure from the duplicate completion being reduced.
  const auto safe = parse_trace(fixture("safe_deferred_reclaim.sgtrace"));
  auto with_violation = safe;
  with_violation.events.insert(with_violation.events.end() - 1,
                               CompleteAccessEvent{OperationId("copy-1")});
  const ReductionOracle double_oracle(with_violation);
  auto lost_completion = with_violation;
  lost_completion.events.erase(lost_completion.events.begin() + 3);
  require(!double_oracle.reproduced_by(lost_completion),
          "oracle accepted a trace whose first completion was removed");
}

void test_inconclusive_reduction() {
  GeneratorConfig config;
  config.fault = Fault::DroppedCompletion;
  config.seed = 3;
  const auto generated = generate_trace(config);
  const auto result = reduce_trace(generated.trace);
  require_valid_reduction(generated.trace, result, "dropped completion");
  require(result.status == CheckStatus::Inconclusive,
          "missing completion must stay inconclusive after reduction");
  require(check(result.trace).status == CheckStatus::Inconclusive,
          "reduced missing-evidence trace was not inconclusive");
}

void test_reduction_errors_and_budget() {
  const auto expect_error = [](const Trace& trace, ReductionOptions options,
                               const std::string& message) {
    bool rejected = false;
    try {
      (void)reduce_trace(trace, options);
    } catch (const ReductionError&) {
      rejected = true;
    }
    require(rejected, message);
  };
  const auto safe = parse_trace(fixture("safe_deferred_reclaim.sgtrace"));
  expect_error(safe, {}, "a clean trace must not be reducible");

  const auto premature = parse_trace(fixture("premature_reclaim.sgtrace"));
  ReductionOptions absent;
  absent.category = DiagnosticCategory::DoubleCompletion;
  expect_error(premature, absent, "an absent category must be rejected");
  ReductionOptions evidence;
  evidence.category = DiagnosticCategory::IncompleteEvidence;
  expect_error(premature, evidence,
               "incomplete evidence cannot be isolated from a definite violation");

  auto unsupported = premature;
  unsupported.schema = 2;
  expect_error(unsupported, {}, "an unsupported schema must be rejected");

  ReductionOptions chosen;
  chosen.category = DiagnosticCategory::PrematureReclaim;
  require(reduce_trace(premature, chosen).target.category == DiagnosticCategory::PrematureReclaim,
          "explicit category was not used");

  GeneratorConfig config;
  config.fault = Fault::PrematureReclaim;
  config.steps = 160;
  const auto generated = generate_trace(config);
  ReductionOptions limited;
  limited.max_checks = 1;
  const auto partial = reduce_trace(generated.trace, limited);
  require(!partial.one_minimal && partial.checks == 1,
          "an exhausted budget must be reported");
  require(ReductionOracle(generated.trace).reproduced_by(partial.trace),
          "a budget-limited reduction must still reproduce the failure");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"generation is deterministic", test_generation_is_deterministic},
      {"legal histories are clean", test_legal_histories_are_clean},
      {"planted faults are reported where planted",
       test_planted_faults_are_reported_where_planted},
      {"round trip preserves generated traces", test_round_trip_preserves_generated_traces},
      {"configuration is validated", test_configuration_is_validated},
      {"reduction preserves planted failures", test_reduction_preserves_planted_failures},
      {"reduction of fixtures", test_reduction_of_fixtures},
      {"oracle rejects manufactured failures", test_oracle_rejects_manufactured_failures},
      {"inconclusive reduction", test_inconclusive_reduction},
      {"reduction errors and budget", test_reduction_errors_and_budget},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
