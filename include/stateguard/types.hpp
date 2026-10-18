#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace stateguard {

// These small wrappers keep semantic identities distinct even when their
// serialized values happen to be identical.  They are deliberately not
// pointers or device addresses.
template <typename Tag>
struct Identifier {
  std::string value;

  Identifier() = default;
  explicit Identifier(std::string v) : value(std::move(v)) {}
  explicit Identifier(std::string_view v) : value(v) {}
  explicit Identifier(const char* v) : value(v == nullptr ? "" : v) {}

  auto operator<=>(const Identifier&) const = default;
  bool empty() const { return value.empty(); }
};

struct StateIdTag;
struct AllocationIdTag;
struct OperationIdTag;
struct RequestIdTag;
struct NamespaceIdTag;
struct EndpointIdTag;

using StateId = Identifier<StateIdTag>;
using AllocationId = Identifier<AllocationIdTag>;
using OperationId = Identifier<OperationIdTag>;
using RequestId = Identifier<RequestIdTag>;
using NamespaceId = Identifier<NamespaceIdTag>;
using EndpointId = Identifier<EndpointIdTag>;
using Generation = std::uint64_t;

struct AllocationRef {
  AllocationId allocation;
  Generation generation = 0;

  auto operator<=>(const AllocationRef&) const = default;
};

struct SemanticDescriptor {
  NamespaceId name_space;
  std::string model_revision;
  std::string adapter;
  std::string component;
  std::string prefix;
  std::uint64_t token_begin = 0;
  std::uint64_t token_end = 0;
  std::string representation;
  std::string layout;
  std::string position_metadata;

  auto operator<=>(const SemanticDescriptor&) const = default;
};

enum class AccessKind { Read, Write };

struct Lease {
  AllocationRef allocation;
  AccessKind access = AccessKind::Read;

  auto operator<=>(const Lease&) const = default;
};

// The backend transfer an operation was handed to: a transport endpoint, the
// endpoint's connection epoch, and the backend's own tag for the transfer.
// Completion evidence from the backend names the same route.
struct TransferRoute {
  EndpointId endpoint;
  std::uint64_t epoch = 0;
  std::string tag;

  auto operator<=>(const TransferRoute&) const = default;
};

struct AllocateEvent {
  AllocationId allocation;
  Generation generation = 0;
  std::uint64_t bytes = 0;
  std::string domain;

  bool operator==(const AllocateEvent&) const = default;
};

struct BindStateEvent {
  AllocationRef allocation;
  StateId state;
  SemanticDescriptor descriptor;

  bool operator==(const BindStateEvent&) const = default;
};

struct SubmitEvent {
  OperationId operation;
  RequestId request;
  std::vector<Lease> leases;
  std::vector<OperationId> dependencies;
  // Set when the backend reports completions with transfer evidence.
  std::optional<TransferRoute> route{};

  bool operator==(const SubmitEvent&) const = default;
};

struct PublishEvent {
  StateId state;
  AllocationRef allocation;
  std::optional<OperationId> completed_operation;

  bool operator==(const PublishEvent&) const = default;
};

struct ConsumeEvent {
  RequestId request;
  StateId state;
  AllocationRef allocation;
  SemanticDescriptor expected;

  bool operator==(const ConsumeEvent&) const = default;
};

struct CancelRequestEvent {
  RequestId request;

  bool operator==(const CancelRequestEvent&) const = default;
};

// How an operation's data access ended. Either way the operation no longer
// touches its leased allocations. A failed write leaves the destination's
// contents incomplete until a later writer succeeds.
enum class AccessOutcome { Succeeded, Failed };

struct CompleteAccessEvent {
  OperationId operation;
  AccessOutcome outcome = AccessOutcome::Succeeded;
  // The transfer the backend's completion named. It must match the
  // operation's route; otherwise the completion belongs to another transfer.
  std::optional<TransferRoute> evidence{};

  bool operator==(const CompleteAccessEvent&) const = default;
};

// A transport endpoint started a new connection epoch, ending the previous
// one. A new epoch does not prove that transfers of an earlier epoch ended.
struct ConnectEndpointEvent {
  EndpointId endpoint;
  std::uint64_t epoch = 0;

  bool operator==(const ConnectEndpointEvent&) const = default;
};

// The host gave up waiting for an operation: a lease expired, a timeout
// fired, or a peer disconnected. The operation's leases stay held, because
// giving up does not prove that the access ended.
struct ExpireOperationEvent {
  OperationId operation;
  std::string reason;

  bool operator==(const ExpireOperationEvent&) const = default;
};

struct RetireEvent {
  AllocationRef allocation;

  bool operator==(const RetireEvent&) const = default;
};

struct ReclaimEvent {
  AllocationRef allocation;

  bool operator==(const ReclaimEvent&) const = default;
};

struct CoverageGapEvent {
  std::string reason;

  bool operator==(const CoverageGapEvent&) const = default;
};

struct EndTraceEvent {
  bool capture_complete = false;

  bool operator==(const EndTraceEvent&) const = default;
};

using Event = std::variant<AllocateEvent, BindStateEvent, SubmitEvent,
                           PublishEvent, ConsumeEvent, CancelRequestEvent,
                           CompleteAccessEvent, RetireEvent, ReclaimEvent,
                           CoverageGapEvent, EndTraceEvent, ConnectEndpointEvent,
                           ExpireOperationEvent>;

struct Trace {
  static constexpr std::uint32_t schema_major = 1;
  std::uint32_t schema = schema_major;
  std::vector<Event> events;
};

}  // namespace stateguard
