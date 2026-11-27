#pragma once

// A cancellation scenario for the timeline, built from the CPU pool and copy
// engine. One process and one clock: it says nothing about how a real engine
// or device schedules a cancellation.

#include "async_copy.hpp"
#include "stateguard/timeline.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace stateguard::harness {

struct CancellationOptions {
  // Stamps the trace. Without a clock function the trace is untimed, which
  // is useful only to measure the cost of the timestamps.
  TraceClock clock;
  // Runs between the cancellation signal and the scheduler's acknowledgment.
  std::function<void()> scheduler_delay{};
  // Runs after the acknowledgment and before the in-flight copy completes:
  // the injected delayed copy.
  std::function<void()> copy_delay{};
  // A second request consumes the shared prefix and finishes normally after
  // the cancellation, so the prefix is never cancelled-only.
  bool shared_live_consumer = false;
  // When false the copy never completes, so the host can never reclaim its
  // buffers and the timeline lacks the completion.
  bool complete_copy = true;
  std::size_t slot_bytes = 4096;
  std::size_t capture_capacity = 256;
};

struct CancellationRun {
  Trace trace;
  CaptureStats capture;
};

// request-1 loads prefix state-1 into a cache buffer, consumes it, and starts
// copying it into an output buffer. Its cancellation is signalled and
// acknowledged while the copy is in flight. When the copy completes, the host
// retires and reclaims the output and the cache buffer (after request-2
// finishes, when it shares the prefix) and the pool hands both slots out again.
CancellationRun run_cancellation(const CancellationOptions& options);

// The first cancelled request of a timeline, reduced to the intervals a demo
// prints. Intervals start at the signal, or at the acknowledgment when the
// trace has no signal, as stateguard-timeline's cancellation_to_reuse does.
// An interval is unset when either end is unknown or the end precedes the
// start.
struct CancellationSummary {
  TimelineStatus status = TimelineStatus::Incomplete;
  std::size_t cancelled_requests = 0;
  // Of the first request.
  std::size_t cancelled_only = 0;
  std::optional<std::uint64_t> signal_to_ack_ns;
  std::optional<std::uint64_t> cancellation_to_eligible_ns;
  std::optional<std::uint64_t> cancellation_to_reuse_ns;
};

CancellationSummary summarize_cancellation(const TimelineReport& report);

// One line, for example "complete, 2 cancelled-only allocations,
// signal_to_ack 300 ns, cancellation_to_eligible 5300 ns,
// cancellation_to_reuse 5300 ns".
std::string to_string(const CancellationSummary& summary);

}  // namespace stateguard::harness
