#include "stateguard/stateguard.hpp"

#include <iostream>

namespace {

stateguard::SemanticDescriptor descriptor() {
  return {stateguard::NamespaceId("demo"), "model-r1", "cpu-cache", "layer-0",
          "prefix-42", 0, 16, "fp16", "row-major", "absolute"};
}

stateguard::Trace safe_trace() {
  using namespace stateguard;
  const AllocationRef buffer{AllocationId("buffer-1"), 1};
  const auto state = StateId("state-1");
  const auto desc = descriptor();
  Trace trace;
  trace.events = {
      AllocateEvent{buffer.allocation, buffer.generation, 4096, "cpu"},
      BindStateEvent{buffer, state, desc},
      SubmitEvent{OperationId("copy-1"), RequestId("request-1"),
                  {Lease{buffer, AccessKind::Write}}, {}},
      CompleteAccessEvent{OperationId("copy-1")},
      PublishEvent{state, buffer, OperationId("copy-1")},
      ConsumeEvent{RequestId("request-1"), state, buffer, desc},
      // Cancellation releases the logical consumer; it does not complete a
      // submitted operation, which is why completion appears above.
      CancelRequestEvent{RequestId("request-1")},
      RetireEvent{buffer},
      ReclaimEvent{buffer},
      EndTraceEvent{true},
  };
  return trace;
}

stateguard::Trace invalid_trace() {
  using namespace stateguard;
  const AllocationRef buffer{AllocationId("buffer-1"), 1};
  const auto state = StateId("state-1");
  const auto desc = descriptor();
  Trace trace;
  trace.events.emplace_back(AllocateEvent{buffer.allocation, buffer.generation, 4096, "cpu"});
  trace.events.emplace_back(BindStateEvent{buffer, state, desc});
  trace.events.emplace_back(SubmitEvent{
      OperationId("copy-1"), RequestId("request-1"),
      {Lease{buffer, AccessKind::Write}}, {}});
  trace.events.emplace_back(CompleteAccessEvent{OperationId("copy-1")});
  trace.events.emplace_back(PublishEvent{state, buffer, OperationId("copy-1")});
  trace.events.emplace_back(ConsumeEvent{RequestId("request-1"), state, buffer, desc});
  trace.events.emplace_back(CancelRequestEvent{RequestId("request-1")});
  // This read operation still holds the generation when reclaim runs.
  trace.events.emplace_back(SubmitEvent{
      OperationId("late-read"), RequestId("request-2"),
      {Lease{buffer, AccessKind::Read}}, {}});
  trace.events.emplace_back(RetireEvent{buffer});
  trace.events.emplace_back(ReclaimEvent{buffer});
  trace.events.emplace_back(EndTraceEvent{true});
  return trace;
}

void print_report(const char* label, const stateguard::CheckReport& report) {
  std::cout << label << ": " << stateguard::to_string(report.status) << '\n';
  for (const auto& diagnostic : report.diagnostics) {
    std::cout << "  " << stateguard::to_string(diagnostic.category) << ": "
              << diagnostic.message << '\n';
  }
}

}  // namespace

int main() {
  print_report("safe deferred reclamation", stateguard::check(safe_trace()));
  print_report("induced premature reclamation", stateguard::check(invalid_trace()));
  return 0;
}
