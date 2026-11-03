#pragma once

#include "stateguard/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace stateguard {

// Cancellation timeline: what happened to the memory a cancelled request
// touched, and when, measured in one process and one clock domain.
//
// It replays a timestamped trace in trace order. It does not check lifetimes;
// run the checker for that. All times are nanoseconds on the trace's clock.

enum class TimelineStatus {
  // Every stage that the trace says should exist was observed with a time.
  Complete,
  // Events or timestamps may be missing: see TimelineReport::incomplete.
  Incomplete,
};

// One generation of an allocation that a cancelled request consumed or
// submitted an operation on.
struct AllocationTimeline {
  AllocationRef allocation;
  std::uint64_t bytes = 0;
  // Every request that consumed it or submitted an operation leasing it.
  std::vector<RequestId> requests;
  // The latest completion of an operation leasing it.
  std::optional<std::uint64_t> last_access_ns;
  // When it was first retired with no outstanding lease and no live consumer,
  // the point from which reclaiming it was safe.
  std::optional<std::uint64_t> eligible_ns;
  std::optional<std::uint64_t> reclaim_ns;
  // When the next generation of the allocation was handed out by the pool.
  std::optional<std::uint64_t> reuse_ns;
  // It was reclaimed before it became eligible; the checker reports why.
  bool reclaimed_before_eligible = false;
  // Cancelled-only retention: while the allocation was not reclaimed, had at
  // least one associated request, and every associated request had been
  // acknowledged as cancelled. A live or finished request keeps it out.
  bool cancelled_only = false;
  std::uint64_t cancelled_only_ns = 0;
  double cancelled_only_byte_seconds = 0;
  // Still cancelled-only at the last timestamp; the totals are lower bounds.
  bool retained_at_end = false;
};

struct RequestTimeline {
  RequestId request;
  // Signal receipt and scheduler acknowledgment. A trace may record either.
  std::optional<std::uint64_t> signal_ns;
  std::optional<std::uint64_t> ack_ns;
  // The request's latest consumption or completion of its own operations.
  // Unset when one of its operations has no observed completion.
  std::optional<std::uint64_t> last_access_ns;
  // Allocations whose retention this cancellation caused, alone or shared
  // only with other cancelled requests. Each is counted once in the totals.
  std::vector<AllocationRef> cancelled_only;
  // Allocations it touched whose retention is not attributed to the
  // cancellation: shared with a live or finished request, or reclaimed
  // before every request using them was cancelled.
  std::vector<AllocationRef> not_attributed;
  // The latest eligibility and reuse among the cancelled-only allocations;
  // unset when one of them did not reach that stage.
  std::optional<std::uint64_t> eligible_ns;
  std::optional<std::uint64_t> reuse_ns;
};

struct TimelineReport {
  TimelineStatus status = TimelineStatus::Incomplete;
  // The trace's clock, and its first and last readings.
  std::string clock;
  std::optional<std::uint64_t> first_ns;
  std::optional<std::uint64_t> last_ns;
  // Requests with a cancellation signal or acknowledgment, in trace order.
  std::vector<RequestTimeline> requests;
  // Allocations those requests touched, in allocation order.
  std::vector<AllocationTimeline> allocations;
  // Deduplicated over allocations: shared bytes are counted once.
  std::uint64_t peak_cancelled_only_bytes = 0;
  double cancelled_only_byte_seconds = 0;
  // Why the timeline is incomplete, one reason per line.
  std::vector<std::string> incomplete;
};

TimelineReport build_timeline(const Trace& trace);

std::string to_string(TimelineStatus status);

}  // namespace stateguard
