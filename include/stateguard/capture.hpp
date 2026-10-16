#pragma once

#include "stateguard/types.hpp"

#include <cstddef>

namespace stateguard {

struct CaptureStats {
  std::size_t capacity = 0;
  // Lifecycle events kept, including coverage gaps recorded by adapters.
  std::size_t recorded = 0;
  // Events refused because the buffer was full. They are all from the tail:
  // after the first refusal the recorder keeps nothing more.
  std::size_t dropped = 0;
  bool finished = false;
};

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

  // Appends the overflow gap, if any, and the end marker, then hands over the
  // trace. Throws std::logic_error when called twice.
  Trace finish();

  CaptureStats stats() const;

 private:
  Trace trace_;
  CaptureStats stats_;
};

}  // namespace stateguard
