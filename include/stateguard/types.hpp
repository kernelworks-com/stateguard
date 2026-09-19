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

using StateId = Identifier<StateIdTag>;
using AllocationId = Identifier<AllocationIdTag>;
using OperationId = Identifier<OperationIdTag>;
using RequestId = Identifier<RequestIdTag>;
using NamespaceId = Identifier<NamespaceIdTag>;
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

struct AllocateEvent {
  AllocationId allocation;
  Generation generation = 0;
  std::uint64_t bytes = 0;
  std::string domain;
};

struct BindStateEvent {
  AllocationRef allocation;
  StateId state;
  SemanticDescriptor descriptor;
};

struct SubmitEvent {
  OperationId operation;
  RequestId request;
  std::vector<Lease> leases;
  std::vector<OperationId> dependencies;
};

struct PublishEvent {
  StateId state;
  AllocationRef allocation;
  std::optional<OperationId> completed_operation;
};

struct ConsumeEvent {
  RequestId request;
  StateId state;
  AllocationRef allocation;
  SemanticDescriptor expected;
};

struct CancelRequestEvent {
  RequestId request;
};

struct CompleteAccessEvent {
  OperationId operation;
};

struct RetireEvent {
  AllocationRef allocation;
};

struct ReclaimEvent {
  AllocationRef allocation;
};

struct CoverageGapEvent {
  std::string reason;
};

struct EndTraceEvent {
  bool capture_complete = false;
};

using Event = std::variant<AllocateEvent, BindStateEvent, SubmitEvent,
                           PublishEvent, ConsumeEvent, CancelRequestEvent,
                           CompleteAccessEvent, RetireEvent, ReclaimEvent,
                           CoverageGapEvent, EndTraceEvent>;

struct Trace {
  static constexpr std::uint32_t schema_major = 1;
  std::uint32_t schema = schema_major;
  std::vector<Event> events;
};

}  // namespace stateguard
