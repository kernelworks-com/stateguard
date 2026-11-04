#include "stateguard/timeline.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

namespace stateguard {
namespace {

using Time = std::optional<std::uint64_t>;

enum class Ending { None, Cancelled, Finished };

struct OperationState {
  RequestId request;
  std::vector<AllocationRef> leases;
  bool completed = false;
};

struct RequestState {
  Time signal;
  Time ack;
  Ending ending = Ending::None;
  bool signalled = false;
  bool acknowledged = false;
  Time last_consume;
  std::vector<OperationId> operations;
  std::set<AllocationRef> allocations;
};

struct AllocationState {
  std::uint64_t bytes = 0;
  std::set<RequestId> requests;
  std::set<RequestId> live_consumers;
  std::size_t outstanding = 0;  // Leases of operations not yet completed.
  bool retired = false;
  bool reclaimed = false;
  bool eligible = false;
  bool reused = false;
  Time last_access;
  Time eligible_at;
  Time reclaim_at;
  Time reuse_at;
  bool premature = false;
  bool cancelled_only = false;  // Currently.
  bool ever_cancelled_only = false;
  Time since;  // Start of the current cancelled-only interval.
  std::uint64_t retained_ns = 0;
  double byte_seconds = 0;
  bool retained_at_end = false;
};

Time later(Time a, Time b) {
  if (!a) return b;
  if (!b) return a;
  return std::max(*a, *b);
}

class Replay {
 public:
  TimelineReport run(const Trace& trace) {
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
      index_ = i;
      std::visit([this](const auto& e) { apply(e); }, trace.events[i]);
    }
    return finish();
  }

 private:
  void reason(std::string text) {
    if (std::find(report_.incomplete.begin(), report_.incomplete.end(), text) ==
        report_.incomplete.end()) {
      report_.incomplete.push_back(std::move(text));
    }
  }

  // The time of the current event, or nothing when it has none.
  Time now() {
    if (!now_ && !clock_broken_ && !untimed_) {
      untimed_ = true;
      reason("event " + std::to_string(index_) + " precedes the first timestamp");
    }
    return now_;
  }

  AllocationState* find(const AllocationRef& ref) {
    const auto found = allocations_.find(ref);
    return found == allocations_.end() ? nullptr : &found->second;
  }

  RequestState& request(const RequestId& id) {
    const auto [it, inserted] = requests_.try_emplace(id);
    return it->second;
  }

  void cancelled_order(const RequestId& id) {
    if (std::find(order_.begin(), order_.end(), id) == order_.end()) order_.push_back(id);
  }

  void associate(const RequestId& id, const AllocationRef& ref, AllocationState& allocation) {
    allocation.requests.insert(id);
    request(id).allocations.insert(ref);
  }

  // Re-evaluates eligibility and cancelled-only retention after a change.
  void update(AllocationState& a) {
    const Time t = now();
    if (!a.eligible && a.retired && !a.reclaimed && a.outstanding == 0 &&
        a.live_consumers.empty()) {
      a.eligible = true;
      a.eligible_at = t;
    }
    const bool cancelled_only =
        !a.reclaimed && !a.requests.empty() &&
        std::all_of(a.requests.begin(), a.requests.end(), [this](const RequestId& id) {
          return requests_.at(id).ending == Ending::Cancelled;
        });
    if (cancelled_only == a.cancelled_only) return;
    a.cancelled_only = cancelled_only;
    if (cancelled_only) {
      a.ever_cancelled_only = true;
      a.since = t;
      current_bytes_ += a.bytes;
      report_.peak_cancelled_only_bytes =
          std::max(report_.peak_cancelled_only_bytes, current_bytes_);
    } else {
      close(a, t);
      current_bytes_ -= a.bytes;
    }
  }

  void close(AllocationState& a, Time t) {
    if (a.since && t && *t >= *a.since) {
      const auto span = *t - *a.since;
      a.retained_ns += span;
      a.byte_seconds += static_cast<double>(a.bytes) * static_cast<double>(span) / 1e9;
    }
    a.since.reset();
  }

  void update_requests_of(const RequestId& id) {
    for (const auto& ref : request(id).allocations) {
      if (auto* a = find(ref)) update(*a);
    }
  }

  void apply(const TimestampEvent& e) {
    if (clock_broken_) return;
    if (report_.clock.empty()) {
      report_.clock = e.clock;
    } else if (e.clock != report_.clock) {
      reason("clock changes from " + report_.clock + " to " + e.clock + " at event " +
             std::to_string(index_) + "; later durations are not reported");
      clock_broken_ = true;
      now_.reset();
      return;
    }
    if (now_ && e.ns < *now_) {
      reason("timestamp decreases at event " + std::to_string(index_) +
             "; later durations are not reported");
      clock_broken_ = true;
      now_.reset();
      return;
    }
    now_ = e.ns;
    if (!report_.first_ns) report_.first_ns = e.ns;
    report_.last_ns = e.ns;
  }

  void apply(const AllocateEvent& e) {
    const AllocationRef ref{e.allocation, e.generation};
    const auto previous = current_.find(e.allocation);
    if (previous != current_.end() && previous->second.generation < e.generation) {
      if (auto* old = find(previous->second); old != nullptr && !old->reused) {
        old->reused = true;
        old->reuse_at = now();
      }
    }
    current_[e.allocation] = ref;
    AllocationState state;
    state.bytes = e.bytes;
    allocations_[ref] = std::move(state);
  }

  void apply(const SubmitEvent& e) {
    auto& r = request(e.request);
    r.operations.push_back(e.operation);
    OperationState op{e.request, {}, false};
    for (const auto& lease : e.leases) {
      op.leases.push_back(lease.allocation);
      if (auto* a = find(lease.allocation)) {
        ++a->outstanding;
        associate(e.request, lease.allocation, *a);
        update(*a);
      }
    }
    operations_.insert_or_assign(e.operation, std::move(op));
  }

  void apply(const ConsumeEvent& e) {
    auto& r = request(e.request);
    r.last_consume = later(r.last_consume, now());
    if (auto* a = find(e.allocation)) {
      associate(e.request, e.allocation, *a);
      if (r.ending == Ending::None) a->live_consumers.insert(e.request);
      update(*a);
    }
  }

  void apply(const CompleteAccessEvent& e) {
    const auto found = operations_.find(e.operation);
    if (found == operations_.end() || found->second.completed) return;
    found->second.completed = true;
    completed_at_[e.operation] = now();
    for (const auto& ref : found->second.leases) {
      if (auto* a = find(ref)) {
        if (a->outstanding != 0) --a->outstanding;
        a->last_access = later(a->last_access, now());
        update(*a);
      }
    }
  }

  void end_request(const RequestId& id, Ending ending) {
    auto& r = request(id);
    if (r.ending == Ending::None) r.ending = ending;
    for (const auto& ref : r.allocations) {
      if (auto* a = find(ref)) a->live_consumers.erase(id);
    }
    update_requests_of(id);
  }

  void apply(const CancelSignalEvent& e) {
    auto& r = request(e.request);
    if (!r.signalled) {
      r.signalled = true;
      r.signal = now();
    }
    cancelled_order(e.request);
  }

  void apply(const CancelRequestEvent& e) {
    auto& r = request(e.request);
    if (!r.acknowledged) {
      r.acknowledged = true;
      r.ack = now();
    }
    cancelled_order(e.request);
    end_request(e.request, Ending::Cancelled);
  }

  void apply(const FinishRequestEvent& e) { end_request(e.request, Ending::Finished); }

  void apply(const RetireEvent& e) {
    if (auto* a = find(e.allocation)) {
      a->retired = true;
      update(*a);
    }
  }

  void apply(const ReclaimEvent& e) {
    if (auto* a = find(e.allocation); a != nullptr && !a->reclaimed) {
      a->reclaimed = true;
      a->reclaim_at = now();
      a->premature = !a->eligible;
      update(*a);
    }
  }

  void apply(const CoverageGapEvent& e) {
    reason("capture coverage gap at event " + std::to_string(index_) + ": " + e.reason);
  }

  void apply(const EndTraceEvent& e) {
    ended_ = true;
    if (!e.capture_complete) reason("trace ended with incomplete capture coverage");
  }

  // Lifetimes of states, endpoints, and expiries do not move the timeline.
  void apply(const BindStateEvent&) {}
  void apply(const PublishEvent&) {}
  void apply(const ConnectEndpointEvent&) {}
  void apply(const ExpireOperationEvent&) {}

  TimelineReport finish() {
    if (!ended_) reason("trace has no end marker");
    if (!report_.first_ns && !clock_broken_) reason("trace has no timestamps");

    std::set<AllocationRef> touched;
    for (const auto& id : order_) {
      const auto& r = requests_.at(id);
      if (r.signalled && !r.acknowledged) {
        reason("request " + id.value +
               " received a cancellation signal but no scheduler acknowledgment");
      }
      touched.insert(r.allocations.begin(), r.allocations.end());
    }
    // An unfinished operation on memory a cancelled request touched leaves its
    // last access, and the memory's eligibility, unknown.
    for (const auto& [id, op] : operations_) {
      if (op.completed) continue;
      const bool relevant =
          std::find(order_.begin(), order_.end(), op.request) != order_.end() ||
          std::any_of(op.leases.begin(), op.leases.end(),
                      [&touched](const AllocationRef& ref) { return touched.contains(ref); });
      if (relevant) {
        reason("operation " + id.value + " of request " + op.request.value +
               " has no observed access completion");
      }
    }

    for (auto& [ref, a] : allocations_) {
      if (a.cancelled_only) {
        if (clock_broken_) {
          a.since.reset();
        } else {
          close(a, report_.last_ns);
        }
        a.retained_at_end = true;
      }
      report_.cancelled_only_byte_seconds += a.byte_seconds;
      if (!touched.contains(ref)) continue;
      AllocationTimeline out;
      out.allocation = ref;
      out.bytes = a.bytes;
      out.requests.assign(a.requests.begin(), a.requests.end());
      out.last_access_ns = a.last_access;
      out.eligible_ns = a.eligible_at;
      out.reclaim_ns = a.reclaim_at;
      out.reuse_ns = a.reuse_at;
      out.reclaimed_before_eligible = a.premature;
      out.cancelled_only = a.ever_cancelled_only;
      out.cancelled_only_ns = a.retained_ns;
      out.cancelled_only_byte_seconds = a.byte_seconds;
      out.retained_at_end = a.retained_at_end;
      report_.allocations.push_back(std::move(out));
    }

    for (const auto& id : order_) {
      const auto& r = requests_.at(id);
      RequestTimeline out;
      out.request = id;
      out.signal_ns = r.signal;
      out.ack_ns = r.ack;
      out.last_access_ns = r.last_consume;
      for (const auto& op : r.operations) {
        if (!operations_.at(op).completed) {
          out.last_access_ns.reset();
          break;
        }
        out.last_access_ns = later(out.last_access_ns, completed_at_.at(op));
      }
      bool all_eligible = true;
      bool all_reused = true;
      for (const auto& ref : r.allocations) {
        const auto& a = allocations_.at(ref);
        if (!a.ever_cancelled_only) {
          out.not_attributed.push_back(ref);
          continue;
        }
        out.cancelled_only.push_back(ref);
        all_eligible = all_eligible && a.eligible_at.has_value();
        all_reused = all_reused && a.reuse_at.has_value();
        out.eligible_ns = later(out.eligible_ns, a.eligible_at);
        out.reuse_ns = later(out.reuse_ns, a.reuse_at);
      }
      if (!all_eligible) out.eligible_ns.reset();
      if (!all_reused) out.reuse_ns.reset();
      report_.requests.push_back(std::move(out));
    }

    report_.status = report_.incomplete.empty() ? TimelineStatus::Complete
                                                : TimelineStatus::Incomplete;
    return std::move(report_);
  }

  TimelineReport report_;
  std::size_t index_ = 0;
  Time now_;
  bool clock_broken_ = false;
  bool untimed_ = false;  // An untimed event was reported.
  bool ended_ = false;
  std::uint64_t current_bytes_ = 0;
  std::map<AllocationRef, AllocationState> allocations_;
  std::map<AllocationId, AllocationRef> current_;
  std::map<OperationId, OperationState> operations_;
  std::map<OperationId, Time> completed_at_;
  std::map<RequestId, RequestState> requests_;
  std::vector<RequestId> order_;
};

}  // namespace

TimelineReport build_timeline(const Trace& trace) { return Replay{}.run(trace); }

std::string to_string(TimelineStatus status) {
  switch (status) {
    case TimelineStatus::Complete: return "complete";
    case TimelineStatus::Incomplete: return "incomplete";
  }
  return "incomplete";
}

}  // namespace stateguard
