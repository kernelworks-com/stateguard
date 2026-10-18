#include "stateguard/reducer.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace stateguard {
namespace {

bool same_finding(const Diagnostic& a, const Diagnostic& b) {
  // Event indices shift as events are removed; the finding itself does not.
  return a.category == b.category && a.message == b.message && a.witness == b.witness &&
         a.confirmed == b.confirmed;
}

bool confirmed_violation(const Diagnostic& diagnostic) {
  return diagnostic.category != DiagnosticCategory::IncompleteEvidence && diagnostic.confirmed;
}

using Keys = std::set<std::string>;

// Identities an event mentions, used both to slice and to group events.
//   allocation:A    every event touching A in any generation, so the history
//                   of a reused allocation stays connected to a stale reference;
//   generation:A#g  every event touching generation g of A;
//   lifecycle:A#g   allocate/bind/publish/retire/reclaim of that generation,
//                   which can often go together while a reference to it stays;
//   endpoint:E      connections of E and every transfer routed through it;
//   route:E:e:t     the submission and completion evidence of one transfer.
Keys keys_of(const Event& event) {
  Keys keys;
  const auto route = [&keys](const TransferRoute& r) {
    keys.insert("endpoint:" + r.endpoint.value);
    keys.insert("route:" + r.endpoint.value + ":" + std::to_string(r.epoch) + ":" + r.tag);
  };
  const auto reference = [&keys](const AllocationRef& ref) {
    keys.insert("allocation:" + ref.allocation.value);
    keys.insert("generation:" + ref.allocation.value + "#" + std::to_string(ref.generation));
  };
  const auto lifecycle = [&keys, &reference](const AllocationRef& ref) {
    reference(ref);
    keys.insert("lifecycle:" + ref.allocation.value + "#" + std::to_string(ref.generation));
  };
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, AllocateEvent>) {
          lifecycle(AllocationRef{e.allocation, e.generation});
        } else if constexpr (std::is_same_v<T, BindStateEvent>) {
          lifecycle(e.allocation);
          keys.insert("state:" + e.state.value);
        } else if constexpr (std::is_same_v<T, SubmitEvent>) {
          keys.insert("operation:" + e.operation.value);
          keys.insert("request:" + e.request.value);
          for (const auto& lease : e.leases) reference(lease.allocation);
          for (const auto& dependency : e.dependencies) {
            keys.insert("operation:" + dependency.value);
          }
          if (e.route) route(*e.route);
        } else if constexpr (std::is_same_v<T, PublishEvent>) {
          keys.insert("state:" + e.state.value);
          lifecycle(e.allocation);
          if (e.completed_operation) keys.insert("operation:" + e.completed_operation->value);
        } else if constexpr (std::is_same_v<T, ConsumeEvent>) {
          keys.insert("request:" + e.request.value);
          keys.insert("state:" + e.state.value);
          reference(e.allocation);
        } else if constexpr (std::is_same_v<T, CancelRequestEvent>) {
          keys.insert("request:" + e.request.value);
        } else if constexpr (std::is_same_v<T, CompleteAccessEvent>) {
          keys.insert("operation:" + e.operation.value);
          if (e.evidence) route(*e.evidence);
        } else if constexpr (std::is_same_v<T, ExpireOperationEvent>) {
          keys.insert("operation:" + e.operation.value);
        } else if constexpr (std::is_same_v<T, ConnectEndpointEvent>) {
          keys.insert("endpoint:" + e.endpoint.value);
        } else if constexpr (std::is_same_v<T, RetireEvent> ||
                             std::is_same_v<T, ReclaimEvent>) {
          lifecycle(e.allocation);
        }
        // Coverage gaps and end markers describe the whole capture.
      },
      event);
  return keys;
}

// Witness lines such as "allocation=buffer-1" name identities the target
// diagnostic depends on even when it is reported at the end marker.
Keys keys_of(const Diagnostic& diagnostic) {
  Keys keys;
  for (const auto& line : diagnostic.witness) {
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const auto name = std::string_view(line).substr(0, separator);
    if (name == "allocation" || name == "operation" || name == "state" ||
        name == "request") {
      keys.insert(std::string(name) + ":" + line.substr(separator + 1));
    } else if (name == "route" || name == "evidence") {
      keys.insert("route:" + line.substr(separator + 1));
    }
  }
  return keys;
}

class Reducer {
 public:
  Reducer(const Trace& original, const ReductionOracle& oracle, std::size_t budget)
      : original_(original), oracle_(oracle), budget_(budget) {
    keys_.reserve(original.events.size());
    for (const auto& event : original.events) keys_.push_back(keys_of(event));
  }

  std::vector<std::size_t> run() {
    std::vector<std::size_t> current(original_.events.size());
    for (std::size_t i = 0; i < current.size(); ++i) current[i] = i;
    current = causal_slice(current);
    // A 1-minimal trace can still lose several scattered events at once, such
    // as an operation's submission with its completion, or one generation's
    // allocate/retire/reclaim. Repeat every pass until none removes anything.
    for (;;) {
      const auto size = current.size();
      current = single_removals(delta_debug(remove_groups(std::move(current))));
      if (current.size() == size || exhausted_) break;
    }
    return current;
  }

  Trace build(const std::vector<std::size_t>& indices) const {
    Trace trace;
    trace.schema = original_.schema;
    trace.events.reserve(indices.size());
    for (const auto index : indices) trace.events.push_back(original_.events[index]);
    return trace;
  }

  std::size_t checks() const { return checks_; }
  bool exhausted() const { return exhausted_; }

 private:
  bool reproduces(const std::vector<std::size_t>& indices) {
    if (checks_ >= budget_) {
      exhausted_ = true;
      return false;
    }
    ++checks_;
    return oracle_.reproduced_by(build(indices));
  }

  static std::vector<std::size_t> without(const std::vector<std::size_t>& from,
                                          const std::vector<std::size_t>& removed) {
    std::vector<std::size_t> result;
    std::set_difference(from.begin(), from.end(), removed.begin(), removed.end(),
                        std::back_inserter(result));
    return result;
  }

  // Keeps events connected to the target through shared identities, plus
  // whole-capture events. Unrelated allocations and requests drop out at once.
  std::vector<std::size_t> causal_slice(const std::vector<std::size_t>& current) {
    const auto target_index = oracle_.target().event_index;
    Keys reached = keys_of(oracle_.target());
    if (target_index < keys_.size()) {
      reached.insert(keys_[target_index].begin(), keys_[target_index].end());
    }
    // With no identity to follow (a coverage gap, for example), the slice is
    // just the whole-capture events.
    std::vector<bool> included(original_.events.size(), false);
    bool grew = true;
    while (grew) {
      grew = false;
      for (const auto index : current) {
        if (included[index]) continue;
        const auto& keys = keys_[index];
        const bool global = keys.empty();
        const bool connected = std::any_of(keys.begin(), keys.end(), [&](const auto& key) {
          return reached.contains(key);
        });
        if (!global && !connected) continue;
        included[index] = true;
        if (connected) {
          for (const auto& key : keys) grew = reached.insert(key).second || grew;
        }
      }
    }
    std::vector<std::size_t> slice;
    for (const auto index : current) {
      if (included[index]) slice.push_back(index);
    }
    if (slice.size() < current.size() && reproduces(slice)) return slice;
    return current;
  }

  // Removes every event that mentions one identity, such as a whole request
  // or a whole allocation history, before falling back to single events.
  std::vector<std::size_t> remove_groups(std::vector<std::size_t> current) {
    bool changed = true;
    while (changed && !exhausted_) {
      changed = false;
      std::map<std::string, std::vector<std::size_t>> groups;
      for (const auto index : current) {
        for (const auto& key : keys_[index]) groups[key].push_back(index);
      }
      for (auto& [key, members] : groups) {
        (void)key;
        // Earlier removals in this pass may already have taken some members.
        std::erase_if(members, [&current](std::size_t index) {
          return !std::binary_search(current.begin(), current.end(), index);
        });
        if (members.size() < 2) continue;  // single events are handled later
        auto candidate = without(current, members);
        if (reproduces(candidate)) {
          current = std::move(candidate);
          changed = true;
        }
      }
    }
    return current;
  }

  // Zeller and Hildebrandt's ddmin, testing complements only: the end marker
  // is required by every reproducing trace, so isolated chunks rarely pass.
  std::vector<std::size_t> delta_debug(std::vector<std::size_t> current) {
    std::size_t granularity = 2;
    while (current.size() >= 2 && !exhausted_) {
      const auto size = current.size();
      granularity = std::min(granularity, size);
      bool reduced = false;
      for (std::size_t chunk = 0; chunk < granularity; ++chunk) {
        const auto begin = chunk * size / granularity;
        const auto end = (chunk + 1) * size / granularity;
        std::vector<std::size_t> candidate(current.begin(), current.begin() + begin);
        candidate.insert(candidate.end(), current.begin() + end, current.end());
        if (reproduces(candidate)) {
          current = std::move(candidate);
          granularity = std::max<std::size_t>(granularity - 1, 2);
          reduced = true;
          break;
        }
      }
      if (reduced) continue;
      if (granularity >= size) break;
      granularity = std::min(granularity * 2, size);
    }
    return current;
  }

  // Guarantees 1-minimality: no single remaining event can be removed.
  std::vector<std::size_t> single_removals(std::vector<std::size_t> current) {
    bool changed = true;
    while (changed && !exhausted_) {
      changed = false;
      for (std::size_t i = current.size(); i-- > 0;) {
        auto candidate = current;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(i));
        if (reproduces(candidate)) {
          current = std::move(candidate);
          changed = true;
        }
      }
    }
    return current;
  }

  const Trace& original_;
  const ReductionOracle& oracle_;
  std::size_t budget_;
  std::vector<Keys> keys_;
  std::size_t checks_ = 0;
  bool exhausted_ = false;
};

}  // namespace

ReductionOracle::ReductionOracle(const Trace& original,
                                 std::optional<DiagnosticCategory> category) {
  if (original.schema != Trace::schema_major) {
    throw ReductionError("unsupported trace schema major version");
  }
  const auto report = check(original);
  if (report.status == CheckStatus::Clean) {
    throw ReductionError("trace is clean; there is no failure to reduce");
  }
  status_ = report.status;
  original_findings_ = report.diagnostics;

  const Diagnostic* target = nullptr;
  if (category) {
    if (*category == DiagnosticCategory::IncompleteEvidence &&
        report.status == CheckStatus::Violation) {
      throw ReductionError(
          "trace has definite violations; incomplete evidence cannot be reduced on its own");
    }
    // Prefer a confirmed finding; an unconfirmed one is reduced only when it
    // is the sole finding of that category.
    for (const auto& diagnostic : report.diagnostics) {
      if (diagnostic.category != *category) continue;
      if (target == nullptr || (!target->confirmed && diagnostic.confirmed)) {
        target = &diagnostic;
      }
      if (target->confirmed) break;
    }
    if (target == nullptr) {
      throw ReductionError("trace reports no " + to_string(*category) + " diagnostic");
    }
  } else {
    for (const auto& diagnostic : report.diagnostics) {
      if (report.status == CheckStatus::Inconclusive || confirmed_violation(diagnostic)) {
        target = &diagnostic;
        break;
      }
    }
  }
  if (target == nullptr) throw ReductionError("trace has no reducible diagnostic");
  target_ = *target;
}

bool ReductionOracle::reproduced_by(const Trace& candidate) const {
  return reproduced_by(check(candidate));
}

bool ReductionOracle::reproduced_by(const CheckReport& candidate) const {
  if (candidate.status != status_) return false;
  bool found = false;
  for (const auto& diagnostic : candidate.diagnostics) {
    const bool known = std::any_of(
        original_findings_.begin(), original_findings_.end(),
        [&diagnostic](const Diagnostic& original) { return same_finding(original, diagnostic); });
    if (!known) return false;
    found = found || same_finding(diagnostic, target_);
  }
  return found;
}

ReductionResult reduce_trace(const Trace& trace, const ReductionOptions& options) {
  const ReductionOracle oracle(trace, options.category);
  Reducer reducer(trace, oracle, options.max_checks);
  ReductionResult result;
  result.kept_events = reducer.run();
  result.trace = reducer.build(result.kept_events);
  result.target = oracle.target();
  result.status = oracle.status();
  result.original_events = trace.events.size();
  result.checks = reducer.checks();
  result.one_minimal = !reducer.exhausted();
  return result;
}

}  // namespace stateguard
