#include "stateguard/capture.hpp"

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace stateguard {

TraceRecorder::TraceRecorder(std::size_t capacity) {
  if (capacity == 0 || capacity > trace_.events.max_size() - 3) {
    throw std::invalid_argument("trace recorder capacity is zero or too large");
  }
  stats_.capacity = capacity;
  // Room for every kept event plus the overflow gap, the end timestamp, and
  // the end marker, so the event buffer is never reallocated.
  trace_.events.reserve(capacity + 3);
}

bool TraceRecorder::record(Event event) {
  if (stats_.finished) {
    throw std::logic_error("event recorded after the trace was finished");
  }
  if (std::holds_alternative<EndTraceEvent>(event)) {
    throw std::invalid_argument("end the trace with TraceRecorder::finish()");
  }
  // Once an event is lost, later events are dropped too: a gap followed by
  // resumed recording would need a gap marker that no longer fits.
  std::optional<std::uint64_t> stamp;
  if (clock_) {
    const auto now = clock_->now_ns();
    if (now != last_ns_) stamp = now;
  }
  const std::size_t needed = stamp ? 2 : 1;
  if (stats_.dropped != 0 || stats_.capacity - stats_.recorded < needed) {
    ++stats_.dropped;
    return false;
  }
  if (stamp) {
    trace_.events.push_back(TimestampEvent{clock_->name, *stamp});
    ++stats_.recorded;
    ++stats_.timestamps;
    last_ns_ = stamp;
  }
  trace_.events.push_back(std::move(event));
  ++stats_.recorded;
  return true;
}

void TraceRecorder::set_clock(TraceClock clock) {
  if (stats_.recorded != 0 || stats_.dropped != 0 || stats_.finished) {
    throw std::logic_error("trace clock set after recording started");
  }
  if (!clock.now_ns || clock.name.empty() ||
      clock.name.find_first_of("|;:,\n\r") != std::string::npos) {
    throw std::invalid_argument("trace clock needs a function and a valid name");
  }
  clock_ = std::move(clock);
}

TraceClock steady_trace_clock() {
  return TraceClock{"steady", [] {
                      return static_cast<std::uint64_t>(
                          std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
                    }};
}

Trace TraceRecorder::finish() {
  if (stats_.finished) throw std::logic_error("trace was already finished");
  stats_.finished = true;
  if (stats_.dropped != 0) {
    trace_.events.emplace_back(CoverageGapEvent{
        "capture buffer full after " + std::to_string(stats_.recorded) +
        " events (" + std::to_string(stats_.dropped) + " dropped)"});
  }
  // The end of capture is stamped so that retention still open at the end is
  // measured up to it.
  if (clock_) {
    const auto now = clock_->now_ns();
    if (now != last_ns_) trace_.events.emplace_back(TimestampEvent{clock_->name, now});
  }
  // The gap marks where events were lost; the end marker says the capture
  // session itself ended here, as in a trace with an adapter-recorded gap.
  trace_.events.emplace_back(EndTraceEvent{true});
  return std::move(trace_);
}

CaptureStats TraceRecorder::stats() const { return stats_; }

LockedTraceRecorder::LockedTraceRecorder(std::size_t capacity) : recorder_(capacity) {}

bool LockedTraceRecorder::record(Event event) {
  const std::lock_guard lock(mutex_);
  return recorder_.record(std::move(event));
}

void LockedTraceRecorder::set_clock(TraceClock clock) {
  const std::lock_guard lock(mutex_);
  recorder_.set_clock(std::move(clock));
}

Trace LockedTraceRecorder::finish() {
  const std::lock_guard lock(mutex_);
  return recorder_.finish();
}

CaptureStats LockedTraceRecorder::stats() const {
  const std::lock_guard lock(mutex_);
  return recorder_.stats();
}

}  // namespace stateguard
