#include "stateguard/stateguard.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace stateguard;

std::string fixture(const char* name) {
  std::ifstream input(std::string(STATEGUARD_SOURCE_DIR) + "/tests/fixtures/" + name);
  if (!input) throw std::runtime_error(std::string("cannot open fixture ") + name);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool has_category(const CheckReport& report, DiagnosticCategory category) {
  for (const auto& diagnostic : report.diagnostics) {
    if (diagnostic.category == category) return true;
  }
  return false;
}

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

CheckReport fixture_report(const char* name) {
  return check(parse_trace(fixture(name)));
}

SemanticDescriptor descriptor() {
  return {NamespaceId("demo"), "model-r1", "cpu-cache", "layer-0", "prefix-42",
          0, 16, "fp16", "row-major", "absolute"};
}

Trace delayed_cancel_trace() {
  const AllocationRef buffer{AllocationId("buffer-1"), 1};
  const auto state = StateId("state-1");
  const auto desc = descriptor();
  Trace trace;
  trace.events = {
      AllocateEvent{buffer.allocation, buffer.generation, 4096, "cpu"},
      BindStateEvent{buffer, state, desc},
      SubmitEvent{OperationId("copy-1"), RequestId("request-1"),
                  {Lease{buffer, AccessKind::Read}}, {}},
      CancelRequestEvent{RequestId("request-1")},
      CompleteAccessEvent{OperationId("copy-1")},
      RetireEvent{buffer},
      ReclaimEvent{buffer},
      EndTraceEvent{true},
  };
  return trace;
}

Trace simple_operation_trace() {
  const AllocationRef buffer{AllocationId("buffer-1"), 1};
  Trace trace;
  trace.events = {
      AllocateEvent{buffer.allocation, buffer.generation, 32, "cpu"},
      SubmitEvent{OperationId("op-1"), RequestId("request-1"),
                  {Lease{buffer, AccessKind::Read}}, {}},
      EndTraceEvent{true},
  };
  return trace;
}

Trace publication_trace(bool include_read_proof, bool include_second_writer,
                        bool publish_before_second_writer_completion) {
  const AllocationRef buffer{AllocationId("buffer-1"), 1};
  const auto state = StateId("state-1");
  const auto desc = descriptor();
  Trace trace;
  trace.events = {
      AllocateEvent{buffer.allocation, buffer.generation, 4096, "cpu"},
      BindStateEvent{buffer, state, desc},
      SubmitEvent{OperationId("writer-1"), RequestId("request-1"),
                  {Lease{buffer, AccessKind::Write}}, {}},
  };
  if (include_second_writer) {
    trace.events.emplace_back(SubmitEvent{
        OperationId("writer-2"), RequestId("request-2"),
        {Lease{buffer, AccessKind::Write}}, {}});
  }
  if (include_read_proof) {
    trace.events.emplace_back(SubmitEvent{
        OperationId("reader-1"), RequestId("request-3"),
        {Lease{buffer, AccessKind::Read}}, {}});
  }
  trace.events.emplace_back(CompleteAccessEvent{OperationId("writer-1")});
  if (include_read_proof) {
    trace.events.emplace_back(CompleteAccessEvent{OperationId("reader-1")});
  }
  if (publish_before_second_writer_completion) {
    trace.events.emplace_back(PublishEvent{state, buffer, OperationId("writer-1")});
    trace.events.emplace_back(CompleteAccessEvent{OperationId("writer-2")});
    trace.events.emplace_back(PublishEvent{state, buffer, OperationId("writer-1")});
  } else {
    if (include_second_writer) {
      trace.events.emplace_back(CompleteAccessEvent{OperationId("writer-2")});
    }
    trace.events.emplace_back(PublishEvent{
        state, buffer,
        include_read_proof ? std::optional<OperationId>(OperationId("reader-1"))
                           : std::optional<OperationId>(OperationId("writer-1"))});
  }
  trace.events.emplace_back(EndTraceEvent{true});
  return trace;
}

void test_positive_fixtures() {
  for (const char* name : {"safe_deferred_reclaim.sgtrace", "shared_prefix.sgtrace"}) {
    const auto report = fixture_report(name);
    require(report.status == CheckStatus::Clean,
            std::string(name) + " should be clean, got " + to_string(report.status));
    require(report.capture_complete, std::string(name) + " should have complete coverage");
  }
}

void test_failure_fixtures() {
  const auto premature = fixture_report("premature_reclaim.sgtrace");
  require(premature.status == CheckStatus::Violation, "premature reclaim must fail");
  require(has_category(premature, DiagnosticCategory::PrematureReclaim),
          "premature reclaim category missing");

  const auto stale = fixture_report("stale_generation.sgtrace");
  require(stale.status == CheckStatus::Violation, "stale generation must fail");
  require(has_category(stale, DiagnosticCategory::StaleGeneration),
          "stale generation category missing");

  const auto incompatible = fixture_report("incompatible_semantics.sgtrace");
  require(incompatible.status == CheckStatus::Violation,
          "incompatible semantic reuse must fail");
  require(has_category(incompatible, DiagnosticCategory::IncompatibleSemanticReuse),
          "incompatible semantic reuse category missing");

  const auto write = fixture_report("write_shared_state.sgtrace");
  require(write.status == CheckStatus::Violation, "shared immutable write must fail");
  require(has_category(write, DiagnosticCategory::WriteThroughSharedImmutableState),
          "shared immutable write category missing");
}

void test_cancellation_does_not_release_access_lease() {
  const auto report = check(delayed_cancel_trace());
  require(report.status == CheckStatus::Clean,
          "delayed completion after cancellation should be safe");
}

void test_missing_completion_is_inconclusive() {
  const auto report = check(simple_operation_trace());
  require(report.status == CheckStatus::Inconclusive,
          "unresolved operation must be inconclusive");
  require(has_category(report, DiagnosticCategory::IncompleteEvidence),
          "missing completion category missing");

  Trace gap;
  gap.events = {CoverageGapEvent{"dropped completion records"}, EndTraceEvent{true}};
  const auto gap_report = check(gap);
  require(gap_report.status == CheckStatus::Inconclusive,
          "capture gap must be inconclusive");
}

void test_publication_requires_writer_completion_proof() {
  const AllocationRef buffer{AllocationId("buffer-1"), 1};
  const auto state = StateId("state-1");
  Trace omitted_proof;
  // Keep writer-1 outstanding at publication and omit the proof entirely.
  omitted_proof.events = {
      AllocateEvent{buffer.allocation, buffer.generation, 4096, "cpu"},
      BindStateEvent{buffer, state, descriptor()},
      SubmitEvent{OperationId("writer-1"), RequestId("request-1"),
                  {Lease{buffer, AccessKind::Write}}, {}},
      PublishEvent{state, buffer, std::nullopt},
      EndTraceEvent{true},
  };
  const auto omitted_report = check(omitted_proof);
  require(omitted_report.status == CheckStatus::Violation,
          "publication with a pending writer and no proof must fail");
  require(has_category(omitted_report, DiagnosticCategory::InvalidStateTransition),
          "pending writer publication category missing");

  const auto completed_report = check(publication_trace(false, false, false));
  require(completed_report.status == CheckStatus::Clean,
          "completed initialization writer publication should be clean");

  const auto unrelated_report = check(publication_trace(true, false, false));
  require(unrelated_report.status == CheckStatus::Violation,
          "unrelated completion proof must fail publication");
  require(has_category(unrelated_report, DiagnosticCategory::InvalidStateTransition),
          "unrelated completion proof category missing");

  const auto pending_second_report = check(publication_trace(false, true, true));
  require(pending_second_report.status == CheckStatus::Violation,
          "publication with a second pending writer must fail");
  require(has_category(pending_second_report, DiagnosticCategory::InvalidStateTransition),
          "multiple writer publication category missing");

  const auto completed_both_report = check(publication_trace(false, true, false));
  require(completed_both_report.status == CheckStatus::Clean,
          "publication after all initialization writers complete should be clean");
}

void test_typed_api_rejects_malformed_identities() {
  Trace empty_allocation;
  empty_allocation.events = {
      AllocateEvent{AllocationId(""), 1, 32, "cpu"}, EndTraceEvent{true}};
  const auto allocation_report = check(empty_allocation);
  require(allocation_report.status == CheckStatus::Violation,
          "typed API must reject an empty allocation identity");
  require(has_category(allocation_report, DiagnosticCategory::InvalidStateTransition),
          "empty allocation identity category missing");

  const AllocationRef buffer{AllocationId("buffer-1"), 1};
  Trace malformed_descriptor;
  malformed_descriptor.events = {
      AllocateEvent{buffer.allocation, buffer.generation, 32, "cpu"},
      BindStateEvent{buffer, StateId("state-1"), SemanticDescriptor{}},
      EndTraceEvent{true},
  };
  const auto descriptor_report = check(malformed_descriptor);
  require(descriptor_report.status == CheckStatus::Violation,
          "typed API must reject an empty semantic descriptor");
  require(has_category(descriptor_report, DiagnosticCategory::InvalidStateTransition),
          "empty descriptor category missing");
}

void test_unknown_access_kind_is_rejected() {
  auto malformed = simple_operation_trace();
  auto& submit = std::get<SubmitEvent>(malformed.events[1]);
  submit.leases[0].access = static_cast<AccessKind>(99);
  const auto report = check(malformed);
  require(report.status == CheckStatus::Violation,
          "unknown access kind must fail the typed checker");
  require(has_category(report, DiagnosticCategory::InvalidStateTransition),
          "unknown access kind category missing");

  bool rejected = false;
  try {
    (void)serialize_trace(malformed);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "serializer must reject unknown access kind");
}

void test_failure_categories() {
  auto duplicate = simple_operation_trace();
  duplicate.events.insert(duplicate.events.end() - 1,
                          CompleteAccessEvent{OperationId("op-1")});
  duplicate.events.insert(duplicate.events.end() - 1,
                          CompleteAccessEvent{OperationId("op-1")});
  const auto duplicate_report = check(duplicate);
  require(has_category(duplicate_report, DiagnosticCategory::DoubleCompletion),
          "duplicate completion category missing");

  Trace orphan;
  orphan.events = {CompleteAccessEvent{OperationId("unknown")}, EndTraceEvent{true}};
  const auto orphan_report = check(orphan);
  require(orphan_report.status == CheckStatus::Violation, "orphan completion must fail");
  require(has_category(orphan_report, DiagnosticCategory::OrphanedOperation),
          "orphan operation category missing");
}

void test_round_trip_and_malformed_input() {
  const auto parsed = parse_trace(fixture("shared_prefix.sgtrace"));
  const auto encoded = serialize_trace(parsed);
  const auto reparsed = parse_trace(encoded);
  require(reparsed.schema == Trace::schema_major, "round-trip schema changed");
  require(reparsed.events.size() == parsed.events.size(), "round-trip event count changed");
  require(check(reparsed).status == CheckStatus::Clean, "round-trip fixture changed meaning");

  bool rejected = false;
  try {
    (void)parse_trace("STATEGUARD_TRACE|2\nend|complete\n");
  } catch (const TraceParseError&) {
    rejected = true;
  }
  require(rejected, "unsupported schema should be rejected");

  rejected = false;
  try {
    (void)parse_trace("STATEGUARD_TRACE|1\nunknown|value\n");
  } catch (const TraceParseError&) {
    rejected = true;
  }
  require(rejected, "unknown event should be rejected");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"positive fixtures", test_positive_fixtures},
      {"failure fixtures", test_failure_fixtures},
      {"cancellation lease", test_cancellation_does_not_release_access_lease},
      {"missing completion", test_missing_completion_is_inconclusive},
      {"publication writer proof", test_publication_requires_writer_completion_proof},
      {"typed API validation", test_typed_api_rejects_malformed_identities},
      {"unknown access kind", test_unknown_access_kind_is_rejected},
      {"failure categories", test_failure_categories},
      {"round trip and malformed input", test_round_trip_and_malformed_input},
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
