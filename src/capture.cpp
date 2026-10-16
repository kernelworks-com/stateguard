#include "stateguard/capture.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace stateguard {

TraceRecorder::TraceRecorder(std::size_t capacity) {
  if (capacity == 0 || capacity > trace_.events.max_size() - 2) {
    throw std::invalid_argument("trace recorder capacity is zero or too large");
  }
  stats_.capacity = capacity;
  // Room for every kept event plus the overflow gap and end marker, so the
  // event buffer is never reallocated while recording.
  trace_.events.reserve(capacity + 2);
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
  if (stats_.dropped != 0 || stats_.recorded == stats_.capacity) {
    ++stats_.dropped;
    return false;
  }
  trace_.events.push_back(std::move(event));
  ++stats_.recorded;
  return true;
}

Trace TraceRecorder::finish() {
  if (stats_.finished) throw std::logic_error("trace was already finished");
  stats_.finished = true;
  if (stats_.dropped != 0) {
    trace_.events.emplace_back(CoverageGapEvent{
        "capture buffer full after " + std::to_string(stats_.recorded) +
        " events (" + std::to_string(stats_.dropped) + " dropped)"});
  }
  // The gap marks where events were lost; the end marker says the capture
  // session itself ended here, as in a trace with an adapter-recorded gap.
  trace_.events.emplace_back(EndTraceEvent{true});
  return std::move(trace_);
}

CaptureStats TraceRecorder::stats() const { return stats_; }

}  // namespace stateguard
