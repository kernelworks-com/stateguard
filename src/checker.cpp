#include "stateguard/checker.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace stateguard {
namespace {

enum class Lifecycle { Reserved, Initializing, Published, Retiring, Free };

struct AllocationRecord {
  Generation generation = 0;
  std::uint64_t bytes = 0;
  std::string domain;
  Lifecycle lifecycle = Lifecycle::Reserved;
  std::optional<StateId> state;
  std::optional<SemanticDescriptor> descriptor;
  std::set<RequestId> consumers;
  // The most recent failed writer of this generation, until a later writer
  // succeeds. The contents are incomplete while it is set.
  std::optional<OperationId> failed_writer;
};

struct StateRecord {
  SemanticDescriptor descriptor;
  bool has_descriptor = false;
  std::set<AllocationRef> publications;
};

struct OperationRecord {
  RequestId request;
  std::vector<Lease> leases;
  bool completed = false;
  bool failed = false;
  std::optional<TransferRoute> route;
  // Why the host stopped waiting for the access, if it did.
  std::optional<std::string> expired;
};

std::string route_text(const TransferRoute& route) {
  return route.endpoint.value + ":" + std::to_string(route.epoch) + ":" + route.tag;
}

struct Replay {
  std::map<AllocationId, AllocationRecord> allocations;
  std::map<AllocationId, Generation> last_generation;
  std::map<StateId, StateRecord> states;
  std::map<OperationId, OperationRecord> operations;
  std::set<RequestId> cancelled;
  std::map<EndpointId, std::uint64_t> endpoints;  // Current epoch of each endpoint.
  std::map<TransferRoute, OperationId> routes;
  CheckReport report;
  bool ended = false;
  std::size_t end_index = 0;
  // Where events may first be missing. Replay is in trace order, so a finding
  // before this point depends only on fully captured events.
  std::optional<std::size_t> first_gap;
  bool unlocated_loss = false;

  void diagnostic(DiagnosticCategory category, std::size_t index,
                  std::string message, std::vector<std::string> witness = {}) {
    report.diagnostics.push_back(
        Diagnostic{category, index, std::move(message), std::move(witness)});
  }

  static bool valid_identifier(std::string_view value) {
    return !value.empty() &&
           value.find_first_of("|;:,\n\r") == std::string_view::npos;
  }

  static bool valid_ref(const AllocationRef& ref) {
    return valid_identifier(ref.allocation.value) && ref.generation != 0;
  }

  static bool valid_route(const TransferRoute& route) {
    return valid_identifier(route.endpoint.value) && route.epoch != 0 &&
           valid_identifier(route.tag);
  }

  static bool valid_access_kind(AccessKind access) {
    return access == AccessKind::Read || access == AccessKind::Write;
  }

  static bool valid_descriptor(const SemanticDescriptor& descriptor) {
    return valid_identifier(descriptor.name_space.value) &&
           valid_identifier(descriptor.model_revision) &&
           valid_identifier(descriptor.adapter) &&
           valid_identifier(descriptor.component) &&
           valid_identifier(descriptor.prefix) &&
           descriptor.token_begin <= descriptor.token_end &&
           valid_identifier(descriptor.representation) &&
           valid_identifier(descriptor.layout) &&
           valid_identifier(descriptor.position_metadata);
  }

  bool validate_ref(const AllocationRef& ref, std::size_t index,
                    std::string_view context) {
    if (valid_ref(ref)) return true;
    diagnostic(DiagnosticCategory::InvalidStateTransition, index,
               std::string(context) + " has an empty or malformed allocation identity");
    return false;
  }

  bool operation_has_writer(const OperationRecord& operation,
                            const AllocationRef& ref) const {
    return std::any_of(operation.leases.begin(), operation.leases.end(),
                       [&ref](const Lease& lease) {
                         return lease.access == AccessKind::Write &&
                                lease.allocation == ref;
                       });
  }

  AllocationRecord* lookup(const AllocationRef& ref, std::size_t index) {
    if (!validate_ref(ref, index, "allocation reference")) return nullptr;
    const auto found = allocations.find(ref.allocation);
    if (found == allocations.end()) {
      const auto previous = last_generation.find(ref.allocation);
      if (previous != last_generation.end() && ref.generation <= previous->second) {
        diagnostic(DiagnosticCategory::StaleGeneration, index,
                   "allocation " + ref.allocation.value + " generation " +
                       std::to_string(ref.generation) +
                       " is no longer live (last generation was " +
                       std::to_string(previous->second) + ")");
      } else {
        diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                   "unknown allocation " + ref.allocation.value);
      }
      return nullptr;
    }
    if (found->second.generation != ref.generation) {
      diagnostic(DiagnosticCategory::StaleGeneration, index,
                 "allocation " + ref.allocation.value + " expected generation " +
                     std::to_string(found->second.generation) + " but event uses " +
                     std::to_string(ref.generation),
                 {"allocation=" + ref.allocation.value,
                  "current_generation=" + std::to_string(found->second.generation),
                  "event_generation=" + std::to_string(ref.generation)});
      return nullptr;
    }
    return &found->second;
  }

  bool is_live_access(const AllocationRecord& allocation) const {
    return allocation.lifecycle != Lifecycle::Free;
  }

  bool lease_outstanding(const AllocationRef& ref) const {
    for (const auto& [operation, record] : operations) {
      (void)operation;
      if (record.completed) continue;
      if (std::any_of(record.leases.begin(), record.leases.end(),
                      [&ref](const Lease& lease) { return lease.allocation == ref; })) {
        return true;
      }
    }
    return false;
  }

  // Why an outstanding operation might look finished to the host although
  // nothing proves that its access ended. Empty when there is no such reason.
  std::string unproven_end(const OperationId& operation, const OperationRecord& record) const {
    std::string reasons;
    if (record.expired) {
      reasons += " (operation " + operation.value + " expired: " + *record.expired +
                 "; expiry does not prove the access ended)";
    }
    if (record.route) {
      const auto endpoint = endpoints.find(record.route->endpoint);
      if (endpoint != endpoints.end() && endpoint->second > record.route->epoch) {
        reasons += " (operation " + operation.value + "'s endpoint " +
                   record.route->endpoint.value + " is now in epoch " +
                   std::to_string(endpoint->second) +
                   "; a new epoch does not prove the transfer ended)";
      }
    }
    return reasons;
  }

  void event(const Event& event, std::size_t index) {
    if (ended) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "event appears after the trace end marker");
      return;
    }

    std::visit(
        [this, index](const auto& value) { apply(value, index); }, event);
  }

  void apply(const AllocateEvent& event, std::size_t index) {
    if (!valid_identifier(event.allocation.value) || event.generation == 0) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "allocation requires a nonempty identity and nonzero generation");
      return;
    }
    const auto active = allocations.find(event.allocation);
    if (active != allocations.end() && active->second.lifecycle != Lifecycle::Free) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "allocation " + event.allocation.value + " is already live");
      return;
    }
    const auto previous = last_generation.find(event.allocation);
    if (previous != last_generation.end() && event.generation <= previous->second) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "allocation generation did not increase on reuse");
      return;
    }
    allocations[event.allocation] =
        AllocationRecord{event.generation, event.bytes, event.domain,
                         Lifecycle::Reserved, std::nullopt, std::nullopt, {}, std::nullopt};
    last_generation[event.allocation] = event.generation;
  }

  void apply(const BindStateEvent& event, std::size_t index) {
    if (!valid_identifier(event.state.value)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "state binding has an empty or malformed state identity");
      return;
    }
    if (!valid_descriptor(event.descriptor)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "state binding has a malformed semantic descriptor");
      return;
    }
    auto* allocation = lookup(event.allocation, index);
    if (allocation == nullptr) return;
    if (allocation->lifecycle != Lifecycle::Reserved &&
        allocation->lifecycle != Lifecycle::Initializing) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "state binding requires a reserved or initializing allocation");
      return;
    }
    auto [state_it, inserted] = states.try_emplace(event.state);
    auto& state = state_it->second;
    if (!state.has_descriptor) {
      state.descriptor = event.descriptor;
      state.has_descriptor = true;
    } else if (state.descriptor != event.descriptor) {
      diagnostic(DiagnosticCategory::IncompatibleSemanticReuse, index,
                 "state " + event.state.value +
                     " was bound with incompatible semantic descriptors");
      return;
    }
    if (allocation->state && *allocation->state != event.state) {
      diagnostic(DiagnosticCategory::IncompatibleSemanticReuse, index,
                 "allocation is already bound to a different logical state");
      return;
    }
    allocation->state = event.state;
    allocation->descriptor = event.descriptor;
    allocation->lifecycle = Lifecycle::Initializing;
    (void)inserted;
  }

  void apply(const SubmitEvent& event, std::size_t index) {
    if (!valid_identifier(event.operation.value) ||
        !valid_identifier(event.request.value) || operations.contains(event.operation)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "operation or request identity is empty, malformed, or was already submitted");
      return;
    }
    if (event.leases.empty()) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "an operation must hold at least one allocation lease");
      return;
    }
    bool valid = true;
    for (const auto& dependency : event.dependencies) {
      const auto found = operations.find(dependency);
      if (found == operations.end()) {
        diagnostic(DiagnosticCategory::OrphanedOperation, index,
                   "operation depends on unknown operation " + dependency.value);
        valid = false;
      } else if (!found->second.completed) {
        diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                   "operation dependency has not completed: " + dependency.value);
        valid = false;
      }
    }
    for (const auto& lease : event.leases) {
      if (!valid_access_kind(lease.access)) {
        diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                   "operation contains an unknown access kind");
        valid = false;
        continue;
      }
      auto* allocation = lookup(lease.allocation, index);
      if (allocation == nullptr) {
        valid = false;
        continue;
      }
      if (!is_live_access(*allocation) || allocation->lifecycle == Lifecycle::Retiring) {
        diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                   "operation submitted against an allocation that is retiring or free");
        valid = false;
      }
      if (lease.access == AccessKind::Write &&
          allocation->lifecycle == Lifecycle::Published) {
        diagnostic(DiagnosticCategory::WriteThroughSharedImmutableState, index,
                   "write lease targets published immutable state " +
                       allocation->state.value_or(StateId{}).value,
                   {"allocation=" + lease.allocation.allocation.value,
                    "generation=" + std::to_string(lease.allocation.generation)});
        valid = false;
      }
    }
    if (event.route && !route_accepted(*event.route, index)) valid = false;
    if (valid) {
      operations.emplace(event.operation,
                         OperationRecord{event.request, event.leases, false, false,
                                         event.route, std::nullopt});
      if (event.route) routes.emplace(*event.route, event.operation);
    }
  }

  bool route_accepted(const TransferRoute& route, std::size_t index) {
    if (!valid_route(route)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "operation has an empty or malformed transfer route");
      return false;
    }
    const auto endpoint = endpoints.find(route.endpoint);
    if (endpoint == endpoints.end()) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "operation submitted on endpoint " + route.endpoint.value +
                     ", which has not connected");
      return false;
    }
    if (endpoint->second != route.epoch) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "operation submitted on endpoint " + route.endpoint.value + " epoch " +
                     std::to_string(route.epoch) + ", but its current epoch is " +
                     std::to_string(endpoint->second));
      return false;
    }
    const auto owner = routes.find(route);
    if (owner != routes.end()) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "transfer route " + route_text(route) + " already belongs to operation " +
                     owner->second.value);
      return false;
    }
    return true;
  }

  void apply(const PublishEvent& event, std::size_t index) {
    if (!valid_identifier(event.state.value)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "publication has an empty or malformed state identity");
      return;
    }
    auto* allocation = lookup(event.allocation, index);
    if (allocation == nullptr) return;
    if (allocation->state != event.state || !allocation->descriptor) {
      diagnostic(DiagnosticCategory::IncompatibleSemanticReuse, index,
                 "published state does not match the allocation's semantic binding");
      return;
    }
    if (allocation->lifecycle == Lifecycle::Published) {
      return;  // Publication notifications are idempotent.
    }
    if (allocation->lifecycle != Lifecycle::Initializing) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "only an initializing allocation can be published");
      return;
    }
    bool proof_valid = true;
    if (event.completed_operation) {
      if (!valid_identifier(event.completed_operation->value)) {
        diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                   "publication completion proof has an empty or malformed operation identity");
        proof_valid = false;
      } else {
        const auto op = operations.find(*event.completed_operation);
        if (op == operations.end()) {
          diagnostic(DiagnosticCategory::OrphanedOperation, index,
                     "publication references an unknown completion operation");
          proof_valid = false;
        } else if (!op->second.completed) {
          diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                     "publication precedes data-access completion");
          proof_valid = false;
        } else if (!operation_has_writer(op->second, event.allocation)) {
          diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                     "publication completion proof is not an initialization writer for this generation");
          proof_valid = false;
        } else if (op->second.failed) {
          failed_publication(event, index, *event.completed_operation,
                             "publication cites failed write " +
                                 event.completed_operation->value);
          proof_valid = false;
        }
      }
    }
    if (proof_valid && allocation->failed_writer) {
      failed_publication(event, index, *allocation->failed_writer,
                         "publication follows failed write " +
                             allocation->failed_writer->value +
                             " with no later successful write");
      proof_valid = false;
    }
    bool pending_writer = false;
    for (const auto& [operation, record] : operations) {
      if (!record.completed && operation_has_writer(record, event.allocation)) {
        pending_writer = true;
        diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                   "publication precedes completion of writer operation " + operation.value,
                   {"allocation=" + event.allocation.allocation.value,
                    "generation=" + std::to_string(event.allocation.generation),
                    "operation=" + operation.value});
      }
    }
    if (!proof_valid || pending_writer) return;
    allocation->lifecycle = Lifecycle::Published;
    states[event.state].publications.insert(event.allocation);
  }

  void failed_publication(const PublishEvent& event, std::size_t index,
                          const OperationId& operation, std::string message) {
    diagnostic(DiagnosticCategory::FailedWritePublication, index,
               std::move(message),
               {"allocation=" + event.allocation.allocation.value,
                "generation=" + std::to_string(event.allocation.generation),
                "operation=" + operation.value});
  }

  void apply(const ConsumeEvent& event, std::size_t index) {
    if (!valid_identifier(event.request.value) ||
        !valid_identifier(event.state.value)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "consumption has an empty or malformed request or state identity");
      return;
    }
    if (!valid_descriptor(event.expected)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "consumption has a malformed expected semantic descriptor");
      return;
    }
    auto* allocation = lookup(event.allocation, index);
    if (allocation == nullptr) return;
    if (allocation->lifecycle != Lifecycle::Published) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "consumption requires a published allocation");
      return;
    }
    if (!allocation->state || *allocation->state != event.state) {
      diagnostic(DiagnosticCategory::IncompatibleSemanticReuse, index,
                 "consumer state identity does not match the published allocation");
      return;
    }
    const auto state = states.find(event.state);
    if (state == states.end() || !state->second.has_descriptor ||
        state->second.descriptor != event.expected ||
        !allocation->descriptor || *allocation->descriptor != event.expected) {
      diagnostic(DiagnosticCategory::IncompatibleSemanticReuse, index,
                 "consumer descriptor does not match the published semantic state",
                 {"state=" + event.state.value,
                  "allocation=" + event.allocation.allocation.value,
                  "generation=" + std::to_string(event.allocation.generation)});
      return;
    }
    allocation->consumers.insert(event.request);
  }

  void apply(const CancelRequestEvent& event, std::size_t index) {
    if (!valid_identifier(event.request.value)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "cancellation has an empty or malformed request identity");
      return;
    }
    (void)index;
    cancelled.insert(event.request);
    for (auto& [allocation_id, allocation] : allocations) {
      (void)allocation_id;
      allocation.consumers.erase(event.request);
    }
  }

  void apply(const CompleteAccessEvent& event, std::size_t index) {
    if (!valid_identifier(event.operation.value)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "completion has an empty or malformed operation identity");
      return;
    }
    const auto found = operations.find(event.operation);
    if (found == operations.end()) {
      diagnostic(DiagnosticCategory::OrphanedOperation, index,
                 "completion references an unknown operation " + event.operation.value);
      return;
    }
    if (event.outcome != AccessOutcome::Succeeded && event.outcome != AccessOutcome::Failed) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "completion has an unknown access outcome");
      return;
    }
    if (event.evidence && !evidence_matches(event, found->second, index)) return;
    if (found->second.completed) {
      diagnostic(DiagnosticCategory::DoubleCompletion, index,
                 "operation " + event.operation.value + " completed more than once");
      return;
    }
    if (found->second.route && !event.evidence) {
      // The completion is accepted, but nothing ties it to this transfer.
      diagnostic(DiagnosticCategory::IncompleteEvidence, index,
                 "completion of " + event.operation.value +
                     " has no backend evidence for transfer " +
                     route_text(*found->second.route),
                 {"operation=" + event.operation.value});
    }
    // Either outcome ends the access, so the operation's leases are released.
    // Only the written contents differ: a failed write leaves them incomplete
    // until a later write to the same generation succeeds.
    found->second.completed = true;
    found->second.failed = event.outcome == AccessOutcome::Failed;
    for (const auto& lease : found->second.leases) {
      if (lease.access != AccessKind::Write) continue;
      const auto allocation = allocations.find(lease.allocation.allocation);
      if (allocation == allocations.end() ||
          allocation->second.generation != lease.allocation.generation) {
        continue;
      }
      if (found->second.failed) {
        allocation->second.failed_writer = event.operation;
      } else {
        allocation->second.failed_writer.reset();
      }
    }
  }

  // A completion whose backend evidence names another transfer proves only
  // that the other transfer ended. The operation's access stays outstanding.
  bool evidence_matches(const CompleteAccessEvent& event, const OperationRecord& record,
                        std::size_t index) {
    const auto& evidence = *event.evidence;
    if (!valid_route(evidence)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "completion has empty or malformed transfer evidence");
      return false;
    }
    if (!record.route) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "completion evidence for " + event.operation.value +
                     ", which was submitted without a transfer route");
      return false;
    }
    const auto& route = *record.route;
    if (evidence == route) return true;
    std::string message = "completion of " + event.operation.value;
    const auto owner = routes.find(evidence);
    if (evidence.endpoint == route.endpoint && evidence.epoch < route.epoch) {
      message += " carries a notification from endpoint " + evidence.endpoint.value +
                 " epoch " + std::to_string(evidence.epoch) + ", but " +
                 event.operation.value + " was submitted in epoch " +
                 std::to_string(route.epoch);
      if (owner != routes.end()) {
        message += " (the notification belongs to operation " + owner->second.value + ")";
      }
    } else if (owner != routes.end()) {
      message += " carries the backend evidence of operation " + owner->second.value;
    } else {
      message += " carries backend evidence for transfer " + route_text(evidence) +
                 ", not its own transfer " + route_text(route);
    }
    diagnostic(DiagnosticCategory::MisattributedCompletion, index, std::move(message),
               {"operation=" + event.operation.value, "route=" + route_text(route),
                "evidence=" + route_text(evidence)});
    return false;
  }

  void apply(const ConnectEndpointEvent& event, std::size_t index) {
    if (!valid_identifier(event.endpoint.value) || event.epoch == 0) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "endpoint connection requires a nonempty identity and nonzero epoch");
      return;
    }
    const auto [endpoint, inserted] = endpoints.try_emplace(event.endpoint, event.epoch);
    if (inserted) return;
    if (event.epoch <= endpoint->second) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "endpoint " + event.endpoint.value + " epoch did not increase");
      return;
    }
    endpoint->second = event.epoch;
  }

  void apply(const ExpireOperationEvent& event, std::size_t index) {
    if (!valid_identifier(event.operation.value) || !valid_identifier(event.reason)) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "expiry has an empty or malformed operation identity or reason");
      return;
    }
    const auto found = operations.find(event.operation);
    if (found == operations.end()) {
      diagnostic(DiagnosticCategory::OrphanedOperation, index,
                 "expiry references an unknown operation " + event.operation.value);
      return;
    }
    // Expiring a finished operation changes nothing. Otherwise the leases stay:
    // only a completion proves that the access ended.
    if (!found->second.completed && !found->second.expired) {
      found->second.expired = event.reason;
    }
  }

  void apply(const RetireEvent& event, std::size_t index) {
    auto* allocation = lookup(event.allocation, index);
    if (allocation == nullptr) return;
    if (allocation->lifecycle == Lifecycle::Retiring) return;  // idempotent.
    if (allocation->lifecycle == Lifecycle::Free) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "a reclaimed allocation cannot be retired again");
      return;
    }
    allocation->lifecycle = Lifecycle::Retiring;
  }

  void apply(const ReclaimEvent& event, std::size_t index) {
    auto* allocation = lookup(event.allocation, index);
    if (allocation == nullptr) return;
    if (allocation->lifecycle == Lifecycle::Free) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "allocation was already reclaimed");
      return;
    }
    if (allocation->lifecycle != Lifecycle::Retiring) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "reclaim requires an explicitly retired allocation");
      return;
    }
    const bool active_access = lease_outstanding(event.allocation);
    const bool live_consumer = !allocation->consumers.empty();
    if (active_access || live_consumer) {
      std::string message = "allocation reclaimed while obligations remain";
      if (active_access) message += " (operation lease is still active)";
      if (live_consumer) message += " (live consumer remains)";
      for (const auto& [operation, record] : operations) {
        if (record.completed) continue;
        if (std::any_of(record.leases.begin(), record.leases.end(),
                        [&event](const Lease& lease) {
                          return lease.allocation == event.allocation;
                        })) {
          message += unproven_end(operation, record);
        }
      }
      diagnostic(DiagnosticCategory::PrematureReclaim, index, std::move(message),
                 {"allocation=" + event.allocation.allocation.value,
                  "generation=" + std::to_string(event.allocation.generation)});
      return;
    }
    allocation->lifecycle = Lifecycle::Free;
  }

  void apply(const CoverageGapEvent& event, std::size_t index) {
    if (!first_gap) first_gap = index;
    diagnostic(DiagnosticCategory::IncompleteEvidence, index,
               "capture coverage gap: " + event.reason);
  }

  void apply(const EndTraceEvent& event, std::size_t index) {
    if (ended) {
      diagnostic(DiagnosticCategory::InvalidStateTransition, index,
                 "trace has more than one end marker");
      return;
    }
    ended = true;
    end_index = index;
    report.capture_complete = event.capture_complete;
    if (!event.capture_complete) {
      // The marker does not say where events were lost, so any point may be
      // affected. A precise capture records a coverage gap at the loss.
      unlocated_loss = true;
      diagnostic(DiagnosticCategory::IncompleteEvidence, index,
                 "trace explicitly ended with incomplete capture coverage");
    }
  }

  void finish() {
    if (!ended) {
      diagnostic(DiagnosticCategory::IncompleteEvidence, 0,
                 "trace has no complete end marker");
    }
    for (const auto& [operation, record] : operations) {
      if (!record.completed) {
        diagnostic(DiagnosticCategory::IncompleteEvidence, end_index,
                   "operation " + operation.value +
                       " has no observed access completion" +
                       unproven_end(operation, record),
                   {"operation=" + operation.value});
      }
    }
    for (auto& d : report.diagnostics) {
      if (d.category == DiagnosticCategory::IncompleteEvidence) continue;
      d.confirmed = !unlocated_loss && !(first_gap && d.event_index > *first_gap);
    }
    // A truncated trace without an end marker lost only its tail; findings in
    // the observed prefix stay confirmed.
    const bool definite = std::any_of(
        report.diagnostics.begin(), report.diagnostics.end(), [](const Diagnostic& d) {
          return d.category != DiagnosticCategory::IncompleteEvidence && d.confirmed;
        });
    if (definite) {
      report.status = CheckStatus::Violation;
    } else if (!report.diagnostics.empty()) {
      report.status = CheckStatus::Inconclusive;
    } else {
      report.status = CheckStatus::Clean;
    }
  }
};

}  // namespace

CheckReport Checker::check(const Trace& trace) const {
  Replay replay;
  if (trace.schema != Trace::schema_major) {
    replay.diagnostic(DiagnosticCategory::MalformedTrace, 0,
                      "unsupported trace schema major version");
    replay.report.status = CheckStatus::Violation;
    return replay.report;
  }
  for (std::size_t index = 0; index < trace.events.size(); ++index) {
    replay.event(trace.events[index], index);
  }
  replay.finish();
  return replay.report;
}

CheckReport check(const Trace& trace) { return Checker{}.check(trace); }

std::string to_string(CheckStatus status) {
  switch (status) {
    case CheckStatus::Clean: return "clean";
    case CheckStatus::Violation: return "violation";
    case CheckStatus::Inconclusive: return "inconclusive";
  }
  return "unknown";
}

std::string to_string(DiagnosticCategory category) {
  switch (category) {
    case DiagnosticCategory::StaleGeneration: return "stale_generation";
    case DiagnosticCategory::IncompatibleSemanticReuse: return "incompatible_semantic_reuse";
    case DiagnosticCategory::WriteThroughSharedImmutableState:
      return "write_through_shared_immutable_state";
    case DiagnosticCategory::PrematureReclaim: return "premature_reclaim";
    case DiagnosticCategory::OrphanedOperation: return "orphaned_operation";
    case DiagnosticCategory::DoubleCompletion: return "double_completion";
    case DiagnosticCategory::FailedWritePublication: return "failed_write_publication";
    case DiagnosticCategory::MisattributedCompletion: return "misattributed_completion";
    case DiagnosticCategory::InvalidStateTransition: return "invalid_state_transition";
    case DiagnosticCategory::IncompleteEvidence: return "incomplete_evidence";
    case DiagnosticCategory::MalformedTrace: return "malformed_trace";
  }
  return "unknown";
}

std::optional<DiagnosticCategory> parse_category(std::string_view name) {
  for (const auto category :
       {DiagnosticCategory::StaleGeneration, DiagnosticCategory::IncompatibleSemanticReuse,
        DiagnosticCategory::WriteThroughSharedImmutableState,
        DiagnosticCategory::PrematureReclaim, DiagnosticCategory::OrphanedOperation,
        DiagnosticCategory::DoubleCompletion, DiagnosticCategory::FailedWritePublication,
        DiagnosticCategory::MisattributedCompletion, DiagnosticCategory::InvalidStateTransition,
        DiagnosticCategory::IncompleteEvidence, DiagnosticCategory::MalformedTrace}) {
    if (to_string(category) == name) return category;
  }
  return std::nullopt;
}

}  // namespace stateguard
