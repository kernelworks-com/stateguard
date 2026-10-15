#include "stateguard/generator.hpp"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <utility>

namespace stateguard {
namespace {

constexpr std::size_t max_steps = 100000;
constexpr std::size_t max_identities = 4096;

// SplitMix64 with rejection sampling. Standard library distributions are not
// specified bit-for-bit, so they would make seeds platform dependent.
class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  std::size_t below(std::size_t bound) {
    if (bound <= 1) return 0;
    const auto limit = static_cast<std::uint64_t>(bound);
    const std::uint64_t threshold = (0 - limit) % limit;
    for (;;) {
      const auto value = next();
      if (value >= threshold) return static_cast<std::size_t>(value % limit);
    }
  }

  bool chance(std::size_t numerator, std::size_t denominator) {
    return below(denominator) < numerator;
  }

  template <typename T>
  const T& pick(const std::vector<T>& values) {
    return values[below(values.size())];
  }

 private:
  std::uint64_t state_;
};

// The generator's own model of the checked lifecycle. Every legal action is
// emitted only when the model says the checker will accept it; a planted fault
// is emitted without changing the model, mirroring the checker, which rejects
// an invalid event without applying it.
enum class Life { Unallocated, Reserved, Initializing, Published, Retiring, Free };

struct Slot {
  AllocationId id;
  Generation generation = 0;
  Life life = Life::Unallocated;
  std::size_t prefix = 0;
  std::set<std::size_t> consumers;
  std::vector<std::pair<Generation, std::size_t>> bindings;
};

struct Operation {
  OperationId id;
  std::vector<Lease> leases;
  bool completed = false;
  bool lost = false;  // Completion is withheld to model capture loss.
};

struct Request {
  RequestId id;
  bool cancelled = false;
};

struct Action {
  std::size_t weight;
  std::function<void()> run;
};

class Builder {
 public:
  explicit Builder(const GeneratorConfig& config) : config_(config), random_(config.seed) {
    // One spare slot is reserved for fault preconditions so that every
    // requested fault can be planted regardless of how the history evolved.
    for (std::size_t i = 0; i <= config.allocations; ++i) {
      slots_.push_back(Slot{AllocationId("alloc-" + std::to_string(i)), 0,
                            Life::Unallocated, 0, {}, {}});
    }
  }

  GeneratedTrace build() {
    GeneratedTrace result;
    result.config = config_;
    result.trace.schema = Trace::schema_major;

    const bool faulted = config_.fault != Fault::None;
    const std::size_t inject_at = faulted ? random_.below(config_.steps + 1) : 0;
    for (std::size_t step = 0; step < config_.steps; ++step) {
      if (faulted && !injected_ && step >= inject_at && try_inject()) continue;
      if (!legal_step()) break;
    }
    if (faulted && !injected_) {
      prepare_fault();
      if (!try_inject()) throw std::logic_error("generator could not plant fault");
    }
    drain();

    const bool complete = config_.fault != Fault::IncompleteEnd;
    emit(EndTraceEvent{complete});
    if (config_.fault == Fault::DroppedCompletion || config_.fault == Fault::IncompleteEnd) {
      fault_event_ = trace_.events.size() - 1;
    }

    result.trace = std::move(trace_);
    result.fault_event = fault_event_;
    result.expected_category = expected_category(config_.fault);
    result.expected_status = expected_status(config_.fault);
    return result;
  }

 private:
  static std::optional<DiagnosticCategory> expected_category(Fault fault) {
    switch (fault) {
      case Fault::None: return std::nullopt;
      case Fault::PrematureReclaim: return DiagnosticCategory::PrematureReclaim;
      case Fault::StaleGeneration: return DiagnosticCategory::StaleGeneration;
      case Fault::IncompatibleSemanticReuse:
        return DiagnosticCategory::IncompatibleSemanticReuse;
      case Fault::WriteThroughSharedImmutableState:
        return DiagnosticCategory::WriteThroughSharedImmutableState;
      case Fault::DoubleCompletion: return DiagnosticCategory::DoubleCompletion;
      case Fault::OrphanedCompletion: return DiagnosticCategory::OrphanedOperation;
      case Fault::PublishBeforeWriterCompletion:
        return DiagnosticCategory::InvalidStateTransition;
      case Fault::DroppedCompletion:
      case Fault::CoverageGap:
      case Fault::IncompleteEnd: return DiagnosticCategory::IncompleteEvidence;
    }
    return std::nullopt;
  }

  static CheckStatus expected_status(Fault fault) {
    switch (fault) {
      case Fault::None: return CheckStatus::Clean;
      case Fault::DroppedCompletion:
      case Fault::CoverageGap:
      case Fault::IncompleteEnd: return CheckStatus::Inconclusive;
      default: return CheckStatus::Violation;
    }
  }

  static SemanticDescriptor descriptor(std::size_t prefix) {
    return {NamespaceId("synthetic"), "model-r1", "base", "layer-0",
            "prefix-" + std::to_string(prefix), 0, 16 * (prefix + 1), "fp16",
            "paged", "absolute"};
  }

  static StateId state(std::size_t prefix) {
    return StateId("state-" + std::to_string(prefix));
  }

  static AllocationRef ref(const Slot& slot) { return {slot.id, slot.generation}; }

  void emit(Event event) { trace_.events.push_back(std::move(event)); }

  void plant(Event event) {
    fault_event_ = trace_.events.size();
    emit(std::move(event));
  }

  // ---- model queries ----

  std::size_t usable_slots() const { return config_.allocations; }

  bool pending_on(const Slot& slot) const {
    const auto current = ref(slot);
    return std::any_of(operations_.begin(), operations_.end(), [&](const Operation& op) {
      return !op.completed &&
             std::any_of(op.leases.begin(), op.leases.end(),
                         [&](const Lease& lease) { return lease.allocation == current; });
    });
  }

  std::vector<std::size_t> writers(const Slot& slot, bool completed) const {
    std::vector<std::size_t> result;
    const auto current = ref(slot);
    for (std::size_t i = 0; i < operations_.size(); ++i) {
      const auto& op = operations_[i];
      if (op.completed != completed) continue;
      for (const auto& lease : op.leases) {
        if (lease.access == AccessKind::Write && lease.allocation == current) {
          result.push_back(i);
          break;
        }
      }
    }
    return result;
  }

  std::vector<std::size_t> slots_in(std::initializer_list<Life> lives,
                                    std::size_t limit) const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < limit; ++i) {
      if (std::find(lives.begin(), lives.end(), slots_[i].life) != lives.end()) {
        result.push_back(i);
      }
    }
    return result;
  }

  std::vector<std::size_t> active_requests() const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < requests_.size(); ++i) {
      if (!requests_[i].cancelled) result.push_back(i);
    }
    return result;
  }

  std::vector<std::size_t> operations_where(bool completed) const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < operations_.size(); ++i) {
      if (operations_[i].completed == completed && !operations_[i].lost) result.push_back(i);
    }
    return result;
  }

  bool can_open_request() const { return requests_.size() < config_.requests; }

  bool request_available() const {
    return can_open_request() || !active_requests().empty();
  }

  // Picks an active request, sometimes opening a new one. Legal actions that
  // need a request are offered only while request_available() holds; fault
  // preconditions may open one request beyond the configured limit.
  std::size_t pick_request() {
    const auto active = active_requests();
    if (active.empty() || (can_open_request() && random_.chance(1, 3))) {
      requests_.push_back(Request{RequestId("req-" + std::to_string(requests_.size())), false});
      return requests_.size() - 1;
    }
    return random_.pick(active);
  }

  // ---- legal actions ----

  void allocate(std::size_t s) {
    auto& slot = slots_[s];
    ++slot.generation;
    slot.life = Life::Reserved;
    slot.consumers.clear();
    emit(AllocateEvent{slot.id, slot.generation, 4096 * (1 + random_.below(4)), "cpu"});
  }

  void bind(std::size_t s, std::size_t prefix) {
    auto& slot = slots_[s];
    slot.life = Life::Initializing;
    slot.prefix = prefix;
    slot.bindings.emplace_back(slot.generation, prefix);
    emit(BindStateEvent{ref(slot), state(prefix), descriptor(prefix)});
  }

  void submit(std::size_t request, std::vector<Lease> leases) {
    std::vector<OperationId> dependencies;
    const auto done = operations_where(true);
    if (!done.empty() && random_.chance(1, 4)) {
      dependencies.push_back(operations_[random_.pick(done)].id);
    }
    Operation op{OperationId("op-" + std::to_string(next_operation_++)), leases, false, false};
    emit(SubmitEvent{op.id, requests_[request].id, std::move(leases), std::move(dependencies)});
    operations_.push_back(std::move(op));
  }

  void submit_writer(std::size_t s, bool allow_copy = true) {
    std::vector<Lease> leases{Lease{ref(slots_[s]), AccessKind::Write}};
    // Model a copy into this allocation from another published allocation.
    auto sources = slots_in({Life::Published}, usable_slots());
    std::erase(sources, s);
    if (allow_copy && !sources.empty() && random_.chance(2, 5)) {
      leases.push_back(Lease{ref(slots_[random_.pick(sources)]), AccessKind::Read});
    }
    submit(pick_request(), std::move(leases));
  }

  void submit_reader(std::size_t s) {
    submit(pick_request(), {Lease{ref(slots_[s]), AccessKind::Read}});
  }

  void complete(std::size_t o) {
    operations_[o].completed = true;
    emit(CompleteAccessEvent{operations_[o].id});
  }

  void publish(std::size_t s) {
    auto& slot = slots_[s];
    std::optional<OperationId> proof;
    const auto done = writers(slot, true);
    if (!done.empty() && random_.chance(2, 3)) proof = operations_[random_.pick(done)].id;
    slot.life = Life::Published;
    emit(PublishEvent{state(slot.prefix), ref(slot), proof});
  }

  void consume(std::size_t s, std::size_t request) {
    auto& slot = slots_[s];
    slot.consumers.insert(request);
    emit(ConsumeEvent{requests_[request].id, state(slot.prefix), ref(slot),
                      descriptor(slot.prefix)});
  }

  void cancel(std::size_t request) {
    requests_[request].cancelled = true;
    for (auto& slot : slots_) slot.consumers.erase(request);
    emit(CancelRequestEvent{requests_[request].id});
  }

  void retire(std::size_t s) {
    slots_[s].life = Life::Retiring;
    emit(RetireEvent{ref(slots_[s])});
  }

  bool reclaimable(const Slot& slot) const {
    return slot.life == Life::Retiring && slot.consumers.empty() && !pending_on(slot);
  }

  void reclaim(std::size_t s) {
    slots_[s].life = Life::Free;
    emit(ReclaimEvent{ref(slots_[s])});
  }

  bool legal_step() {
    std::vector<Action> actions;
    const bool requests = request_available();
    for (std::size_t s = 0; s < usable_slots(); ++s) {
      const auto& slot = slots_[s];
      switch (slot.life) {
        case Life::Unallocated:
        case Life::Free:
          actions.push_back({3, [this, s] { allocate(s); }});
          break;
        case Life::Reserved:
          actions.push_back({4, [this, s] { bind(s, random_.below(config_.prefixes)); }});
          actions.push_back({1, [this, s] { retire(s); }});
          break;
        case Life::Initializing:
          if (requests) actions.push_back({4, [this, s] { submit_writer(s); }});
          if (writers(slot, false).empty() && !writers(slot, true).empty()) {
            actions.push_back({5, [this, s] { publish(s); }});
          }
          actions.push_back({1, [this, s] { retire(s); }});
          break;
        case Life::Published:
          if (requests) {
            actions.push_back({4, [this, s] { consume(s, pick_request()); }});
            actions.push_back({3, [this, s] { submit_reader(s); }});
          }
          actions.push_back({2, [this, s] { retire(s); }});
          // Duplicate publication notifications are legal and idempotent.
          actions.push_back({1, [this, s] {
                               emit(PublishEvent{state(slots_[s].prefix), ref(slots_[s]),
                                                 std::nullopt});
                             }});
          break;
        case Life::Retiring:
          // Duplicate retirement is legal and idempotent.
          actions.push_back({1, [this, s] { retire(s); }});
          if (reclaimable(slot)) actions.push_back({5, [this, s] { reclaim(s); }});
          break;
      }
    }
    for (const auto o : operations_where(false)) {
      actions.push_back({5, [this, o] { complete(o); }});
    }
    for (const auto r : active_requests()) {
      actions.push_back({1, [this, r] { cancel(r); }});
    }
    if (actions.empty()) return false;

    std::size_t total = 0;
    for (const auto& action : actions) total += action.weight;
    auto roll = random_.below(total);
    for (const auto& action : actions) {
      if (roll < action.weight) {
        action.run();
        return true;
      }
      roll -= action.weight;
    }
    return false;
  }

  void drain() {
    auto pending = operations_where(false);
    // Fisher-Yates with the deterministic generator: completions arrive in an
    // order unrelated to submission, as asynchronous work does.
    for (std::size_t i = pending.size(); i > 1; --i) {
      std::swap(pending[i - 1], pending[random_.below(i)]);
    }
    for (const auto o : pending) complete(o);
    for (const auto r : active_requests()) cancel(r);
    for (std::size_t s = 0; s < slots_.size(); ++s) {
      const auto life = slots_[s].life;
      if (life == Life::Reserved || life == Life::Initializing || life == Life::Published) {
        retire(s);
      }
    }
    for (std::size_t s = 0; s < slots_.size(); ++s) {
      if (reclaimable(slots_[s])) reclaim(s);
    }
  }

  // ---- fault planting ----

  std::size_t spare() const { return config_.allocations; }

  // Drives the spare slot through legal events until the fault's precondition
  // holds. Used only when the random history never offered an opportunity.
  void prepare_fault() {
    const auto s = spare();
    allocate(s);
    switch (config_.fault) {
      case Fault::StaleGeneration:
        retire(s);
        reclaim(s);
        allocate(s);
        return;
      case Fault::OrphanedCompletion:
      case Fault::CoverageGap:
      case Fault::IncompleteEnd:
      case Fault::None:
        return;
      default:
        break;
    }
    bind(s, random_.below(config_.prefixes));
    submit_writer(s, false);
    if (config_.fault == Fault::PrematureReclaim ||
        config_.fault == Fault::PublishBeforeWriterCompletion ||
        config_.fault == Fault::DroppedCompletion) {
      return;
    }
    complete(operations_.size() - 1);
    if (config_.fault == Fault::DoubleCompletion) return;
    publish(s);
  }

  bool try_inject() {
    const auto all = slots_.size();
    switch (config_.fault) {
      case Fault::None:
        return false;
      case Fault::PrematureReclaim: {
        std::vector<std::size_t> held;
        for (std::size_t s = 0; s < all; ++s) {
          const auto& slot = slots_[s];
          if (slot.life == Life::Unallocated || slot.life == Life::Free) continue;
          if (pending_on(slot) || !slot.consumers.empty()) held.push_back(s);
        }
        if (held.empty()) return false;
        const auto s = random_.pick(held);
        if (slots_[s].life != Life::Retiring) retire(s);
        plant(ReclaimEvent{ref(slots_[s])});
        break;
      }
      case Fault::StaleGeneration: {
        std::vector<std::size_t> reused;
        for (std::size_t s = 0; s < all; ++s) {
          if (slots_[s].generation >= 2) reused.push_back(s);
        }
        if (reused.empty()) return false;
        const auto& slot = slots_[random_.pick(reused)];
        const Generation old = 1 + random_.below(slot.generation - 1);
        std::size_t prefix = 0;
        for (const auto& [generation, bound] : slot.bindings) {
          if (generation == old) prefix = bound;
        }
        plant(ConsumeEvent{requests_[pick_request()].id, state(prefix),
                           AllocationRef{slot.id, old}, descriptor(prefix)});
        break;
      }
      case Fault::IncompatibleSemanticReuse: {
        const auto published = slots_in({Life::Published}, all);
        if (published.empty()) return false;
        const auto& slot = slots_[random_.pick(published)];
        auto expected = descriptor(slot.prefix);
        if (random_.chance(1, 2)) expected.model_revision = "model-r2";
        else expected.token_end += 1;
        plant(ConsumeEvent{requests_[pick_request()].id, state(slot.prefix), ref(slot),
                           expected});
        break;
      }
      case Fault::WriteThroughSharedImmutableState: {
        const auto published = slots_in({Life::Published}, all);
        if (published.empty()) return false;
        const auto& slot = slots_[random_.pick(published)];
        // The checker rejects this submission, so the identity is never reused.
        plant(SubmitEvent{OperationId("op-" + std::to_string(next_operation_++)),
                          requests_[pick_request()].id,
                          {Lease{ref(slot), AccessKind::Write}}, {}});
        break;
      }
      case Fault::DoubleCompletion: {
        const auto done = operations_where(true);
        if (done.empty()) return false;
        plant(CompleteAccessEvent{operations_[random_.pick(done)].id});
        break;
      }
      case Fault::OrphanedCompletion:
        plant(CompleteAccessEvent{OperationId("op-orphan")});
        break;
      case Fault::PublishBeforeWriterCompletion: {
        std::vector<std::size_t> writing;
        for (const auto s : slots_in({Life::Initializing}, all)) {
          if (!writers(slots_[s], false).empty()) writing.push_back(s);
        }
        if (writing.empty()) return false;
        const auto& slot = slots_[random_.pick(writing)];
        plant(PublishEvent{state(slot.prefix), ref(slot), std::nullopt});
        break;
      }
      case Fault::DroppedCompletion: {
        const auto pending = operations_where(false);
        if (pending.empty()) return false;
        // No event is emitted: the completion simply never reaches the trace.
        operations_[random_.pick(pending)].lost = true;
        break;
      }
      case Fault::CoverageGap:
        plant(CoverageGapEvent{"injected capture loss"});
        break;
      case Fault::IncompleteEnd:
        break;  // Planted by the end marker.
    }
    injected_ = true;
    return true;
  }

  GeneratorConfig config_;
  Random random_;
  Trace trace_;
  std::vector<Slot> slots_;
  std::vector<Operation> operations_;
  std::vector<Request> requests_;
  std::size_t next_operation_ = 0;
  bool injected_ = false;
  std::optional<std::size_t> fault_event_;
};

void validate(const GeneratorConfig& config) {
  if (config.steps > max_steps) {
    throw std::invalid_argument("steps must be at most " + std::to_string(max_steps));
  }
  const auto in_range = [](std::size_t value) {
    return value >= 1 && value <= max_identities;
  };
  if (!in_range(config.allocations) || !in_range(config.prefixes) ||
      !in_range(config.requests)) {
    throw std::invalid_argument("allocations, prefixes, and requests must be between 1 and " +
                                std::to_string(max_identities));
  }
  if (!parse_fault(to_string(config.fault))) {
    throw std::invalid_argument("unknown fault kind");
  }
}

}  // namespace

GeneratedTrace generate_trace(const GeneratorConfig& config) {
  validate(config);
  return Builder(config).build();
}

const std::vector<Fault>& fault_kinds() {
  static const std::vector<Fault> kinds = {
      Fault::None,
      Fault::PrematureReclaim,
      Fault::StaleGeneration,
      Fault::IncompatibleSemanticReuse,
      Fault::WriteThroughSharedImmutableState,
      Fault::DoubleCompletion,
      Fault::OrphanedCompletion,
      Fault::PublishBeforeWriterCompletion,
      Fault::DroppedCompletion,
      Fault::CoverageGap,
      Fault::IncompleteEnd,
  };
  return kinds;
}

std::string to_string(Fault fault) {
  switch (fault) {
    case Fault::None: return "none";
    case Fault::PrematureReclaim: return "premature_reclaim";
    case Fault::StaleGeneration: return "stale_generation";
    case Fault::IncompatibleSemanticReuse: return "incompatible_semantic_reuse";
    case Fault::WriteThroughSharedImmutableState:
      return "write_through_shared_immutable_state";
    case Fault::DoubleCompletion: return "double_completion";
    case Fault::OrphanedCompletion: return "orphaned_completion";
    case Fault::PublishBeforeWriterCompletion: return "publish_before_writer_completion";
    case Fault::DroppedCompletion: return "dropped_completion";
    case Fault::CoverageGap: return "coverage_gap";
    case Fault::IncompleteEnd: return "incomplete_end";
  }
  return "unknown";
}

std::optional<Fault> parse_fault(std::string_view name) {
  for (const auto fault : fault_kinds()) {
    if (to_string(fault) == name) return fault;
  }
  return std::nullopt;
}

}  // namespace stateguard
