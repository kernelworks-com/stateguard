#pragma once

#include "stateguard/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace stateguard {

struct CaptureStats {
  std::size_t capacity = 0;
  // Lifecycle events kept, including coverage gaps recorded by adapters.
  std::size_t recorded = 0;
  // Events refused because the buffer was full. They are all from the tail:
  // after the first refusal the recorder keeps nothing more.
  std::size_t dropped = 0;
  bool finished = false;
  // Of the recorded events, the timestamps the recorder added.
  std::size_t timestamps = 0;
};

// A monotonic clock for stamping captured events. `name` identifies the clock
// domain in the trace; readings of different clocks are not comparable.
struct TraceClock {
  std::string name;
  std::function<std::uint64_t()> now_ns;
};

// std::chrono::steady_clock in nanoseconds, named "steady".
TraceClock steady_trace_clock();

// Collects events from instrumented code into a bounded buffer and produces a
// trace for the checker.
//
// Ordering: the trace order is the order of record() calls, so an adapter must
// record an event before it makes the event's effect visible to the rest of
// the program. Record an access completion before posting the notification
// that lets the host release memory; record a reclaim before the memory can be
// handed out again.
//
// Bounded memory: at most `capacity` events are kept. The first event that does
// not fit, and every event after it, is dropped and counted. finish() then
// records a coverage gap at that point, so findings before the loss stay
// confirmed and the trace cannot be reported clean.
//
// Timestamps: with a clock set, record() reads it and, when the reading differs
// from the last recorded one, records a TimestampEvent before the event. The
// two are kept or dropped together. By the ordering rule the reading precedes
// the event's visible effect; it is when the adapter recorded the event, not
// when a device performed it. finish() stamps the end of capture too, outside
// the capacity.
//
// Reentrancy: record() never calls back into instrumented code. Its storage
// uses the standard allocator, so the recorder must not be used to instrument
// operator new or malloc themselves. It is not thread-safe; capture from one
// thread, or serialize calls externally in an order that respects the rule
// above.
class TraceRecorder {
 public:
  // Reserves the whole buffer up front. Throws std::invalid_argument when
  // capacity is zero or too large to reserve.
  explicit TraceRecorder(std::size_t capacity);

  // Returns false when the event was dropped. Throws std::invalid_argument for
  // an EndTraceEvent (use finish()) and std::logic_error after finish().
  bool record(Event event);

  // Stamps every later event. Throws std::logic_error once an event has been
  // recorded and std::invalid_argument for an empty function or a name that
  // is not a valid identifier.
  void set_clock(TraceClock clock);

  // Appends the overflow gap, if any, and the end marker, then hands over the
  // trace. Throws std::logic_error when called twice.
  Trace finish();

  CaptureStats stats() const;

 private:
  Trace trace_;
  CaptureStats stats_;
  std::optional<TraceClock> clock_;
  std::optional<std::uint64_t> last_ns_;
};

}  // namespace stateguard
