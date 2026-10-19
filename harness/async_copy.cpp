#include "async_copy.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace stateguard::harness {

AllocationRef to_ref(const BufferHandle& handle) {
  return {AllocationId("slot-" + std::to_string(handle.slot)), handle.generation};
}

BufferPool::BufferPool(TraceRecorder& recorder, std::size_t slots,
                       std::size_t slot_bytes)
    : recorder_(recorder), slot_bytes_(slot_bytes), slots_(slots) {
  if (slots == 0 || slot_bytes == 0) {
    throw std::invalid_argument("buffer pool needs at least one nonempty slot");
  }
  for (auto& slot : slots_) slot.storage.resize(slot_bytes);
  // Hand out slot 0 first.
  for (std::size_t i = slots; i > 0; --i) free_.push_back(i - 1);
}

BufferPool::Slot& BufferPool::checked(const BufferHandle& handle, const char* action) {
  if (handle.slot >= slots_.size() || slots_[handle.slot].generation != handle.generation ||
      slots_[handle.slot].state == SlotState::Free) {
    throw std::logic_error(std::string(action) + ": stale or free handle slot-" +
                           std::to_string(handle.slot) + " generation " +
                           std::to_string(handle.generation));
  }
  return slots_[handle.slot];
}

BufferHandle BufferPool::acquire() {
  if (free_.empty()) throw std::runtime_error("buffer pool exhausted");
  const auto index = free_.back();
  auto& slot = slots_[index];
  // Generations never wrap: a wrapped generation could match a stale handle.
  if (slot.generation == std::numeric_limits<Generation>::max()) {
    throw std::runtime_error("slot generation exhausted");
  }
  free_.pop_back();
  ++slot.generation;
  slot.state = SlotState::Live;
  const BufferHandle handle{index, slot.generation};
  const auto ref = to_ref(handle);
  recorder_.record(AllocateEvent{ref.allocation, ref.generation, slot_bytes_, "cpu"});
  return handle;
}

void BufferPool::retire(const BufferHandle& handle) {
  auto& slot = checked(handle, "retire");
  if (slot.state != SlotState::Live) {
    throw std::logic_error("retire: buffer is already retired");
  }
  recorder_.record(RetireEvent{to_ref(handle)});
  slot.state = SlotState::Retiring;
}

void BufferPool::reclaim(const BufferHandle& handle) {
  auto& slot = checked(handle, "reclaim");
  if (slot.state != SlotState::Retiring) {
    throw std::logic_error("reclaim: buffer was not retired");
  }
  // Recorded before the slot can be handed out again.
  recorder_.record(ReclaimEvent{to_ref(handle)});
  slot.state = SlotState::Free;
  free_.push_back(handle.slot);
}

std::span<std::byte> BufferPool::bytes(const BufferHandle& handle) {
  return checked(handle, "bytes").storage;
}

std::byte* BufferPool::address(const BufferHandle& handle) {
  return checked(handle, "address").storage.data();
}

CopyEngine::CopyEngine(TraceRecorder& recorder, BufferPool& pool, EngineOptions options)
    : recorder_(recorder), pool_(pool), options_(options) {
  if (!options_.reports_completion) {
    recorder_.record(CoverageGapEvent{"copy engine does not report access completion"});
  }
  if (options_.endpoint) {
    if (!options_.reports_completion) {
      throw std::invalid_argument("a routed engine reports completion through notifications");
    }
    epoch_ = 1;
    recorder_.record(ConnectEndpointEvent{*options_.endpoint, epoch_});
  }
}

std::optional<TransferRoute> CopyEngine::submit(const OperationId& operation,
                                                const RequestId& request,
                                                const BufferHandle& source,
                                                const BufferHandle& destination) {
  if (submitted_.contains(operation)) {
    throw std::logic_error("operation identity reused: " + operation.value);
  }
  if (source.slot == destination.slot) {
    throw std::logic_error("copy source and destination share a slot");
  }
  Job job{operation, source, destination, pool_.address(source),
          pool_.address(destination)};
  if (options_.endpoint) {
    job.route = TransferRoute{*options_.endpoint, epoch_, std::to_string(next_tag_++)};
  }
  recorder_.record(SubmitEvent{operation, request,
                               {Lease{to_ref(source), AccessKind::Read},
                                Lease{to_ref(destination), AccessKind::Write}},
                               {},
                               job.route});
  submitted_.insert(operation);
  auto route = job.route;
  jobs_.push_back(std::move(job));
  return route;
}

void CopyEngine::complete(const OperationId& operation) {
  const auto job = std::find_if(jobs_.begin(), jobs_.end(),
                                [&](const Job& j) { return j.operation == operation; });
  if (job == jobs_.end()) {
    throw std::logic_error("operation is not pending: " + operation.value);
  }
  // The addresses were resolved at submission. If the host reclaimed a slot
  // early, this copy touches whatever generation owns it now.
  const bool fails = options_.failing_operations.contains(operation.value);
  std::memmove(job->to, job->from, fails ? pool_.slot_bytes() / 2 : pool_.slot_bytes());
  const auto outcome = fails ? AccessOutcome::Failed : AccessOutcome::Succeeded;
  if (options_.reports_completion && !options_.endpoint) {
    recorder_.record(CompleteAccessEvent{operation, outcome});
  }
  if (fails) failed_.insert(operation);
  const Notification notification{operation, job->route, outcome};
  jobs_.erase(job);
  notifications_.push_back(notification);
  if (options_.duplicate_notifications) notifications_.push_back(notification);
  // A late notification of an earlier epoch belongs to that epoch's replay set.
  if (notification.route && notification.route->epoch == epoch_) {
    posted_in_epoch_.push_back(notification);
  }
}

void CopyEngine::reconnect() {
  if (!options_.endpoint) throw std::logic_error("reconnect: engine has no endpoint");
  ++epoch_;
  next_tag_ = 1;
  recorder_.record(ConnectEndpointEvent{*options_.endpoint, epoch_});
  if (options_.redeliver_on_reconnect) {
    for (const auto& notification : posted_in_epoch_) notifications_.push_back(notification);
  }
  posted_in_epoch_.clear();
}

std::optional<Notification> CopyEngine::poll() {
  if (notifications_.empty()) return std::nullopt;
  auto notification = std::move(notifications_.front());
  notifications_.pop_front();
  return notification;
}

std::vector<OperationId> CopyEngine::pending() const {
  std::vector<OperationId> result;
  for (const auto& job : jobs_) result.push_back(job.operation);
  return result;
}

bool CopyEngine::accessing(const BufferHandle& handle) const {
  return std::any_of(jobs_.begin(), jobs_.end(), [&](const Job& job) {
    return job.source == handle || job.destination == handle;
  });
}

CacheHost::CacheHost(TraceRecorder& recorder, BufferPool& pool, CopyEngine& engine,
                     HostOptions options)
    : recorder_(recorder), pool_(pool), engine_(engine), options_(options) {}

BufferHandle CacheHost::allocate() {
  const auto buffer = pool_.acquire();
  buffers_[buffer] = BufferRecord{};
  return buffer;
}

void CacheHost::fill(const BufferHandle& buffer, std::byte value) {
  const auto bytes = pool_.bytes(buffer);
  std::fill(bytes.begin(), bytes.end(), value);
}

void CacheHost::load(const OperationId& operation, const BufferHandle& staging,
                     const BufferHandle& cache, const StateId& state,
                     const SemanticDescriptor& descriptor) {
  if (!buffers_.contains(cache)) throw std::logic_error("load: cache buffer is not live");
  recorder_.record(BindStateEvent{to_ref(cache), state, descriptor});
  const auto staged = pool_.bytes(staging);
  OperationRecord record{staging, cache, state, false, {staged.begin(), staged.end()}};
  record.descriptor = descriptor;
  record.route = engine_.submit(operation, RequestId("loader"), staging, cache);
  track(operation, std::move(record));
}

void CacheHost::consume(const RequestId& request, const StateId& state,
                        const BufferHandle& cache, const SemanticDescriptor& descriptor) {
  if (!published(state)) throw std::logic_error("consume: state is not published");
  recorder_.record(ConsumeEvent{request, state, to_ref(cache), descriptor});
  hold(cache);
  consumers_[request].push_back(cache);
}

void CacheHost::send(const OperationId& operation, const RequestId& request,
                     const BufferHandle& source, const BufferHandle& destination) {
  OperationRecord record{source, destination, std::nullopt, false, {}};
  record.route = engine_.submit(operation, request, source, destination);
  track(operation, std::move(record));
}

void CacheHost::track(const OperationId& operation, OperationRecord record) {
  if (record.route) {
    routes_[*record.route] = operation;
    tags_[record.route->tag] = operation;
  }
  hold(record.source);
  hold(record.destination);
  operations_[operation] = std::move(record);
}

std::optional<OperationId> CacheHost::attribute(const TransferRoute& route) const {
  if (options_.attribution == Attribution::TagOnly) {
    const auto found = tags_.find(route.tag);
    if (found == tags_.end()) return std::nullopt;
    return found->second;
  }
  const auto found = routes_.find(route);
  if (found == routes_.end()) return std::nullopt;
  auto operation = found->second;
  if (options_.attribution == Attribution::LatestAttempt) {
    while (const auto& next = operations_.at(operation).retried_as) operation = *next;
  }
  return operation;
}

void CacheHost::end_request(const RequestId& request) {
  recorder_.record(CancelRequestEvent{request});
  const auto found = consumers_.find(request);
  if (found == consumers_.end()) return;
  const auto held = std::move(found->second);
  consumers_.erase(found);
  for (const auto& buffer : held) drop(buffer);
}

void CacheHost::release(const BufferHandle& buffer) {
  pool_.retire(buffer);
  auto& record = buffers_.at(buffer);
  record.released = true;
  if (options_.reclaim == ReclaimPolicy::Immediate || record.holds == 0) reclaim(buffer);
}

void CacheHost::poll() {
  while (const auto notification = engine_.poll()) {
    // A routed host knows only the route the notification names.
    const auto operation =
        notification->route ? attribute(*notification->route) : notification->operation;
    if (!operation) continue;
    const auto found = operations_.find(*operation);
    if (found == operations_.end()) continue;
    auto& record = found->second;
    if (record.notified && options_.idempotent_notifications) continue;
    const bool first = !record.notified;
    record.notified = true;
    if (notification->route) {
      // The adapter records the completion with the route the backend named.
      recorder_.record(
          CompleteAccessEvent{*operation, notification->outcome, notification->route});
      if (*operation != notification->operation) ++misattributions_;
    }
    // Publish a loaded state only while its buffer is still wanted; a buffer
    // released during the load is retiring and must not become readable.
    const auto cache = buffers_.find(record.destination);
    if (first && record.publishes && cache != buffers_.end() && !cache->second.released) {
      const bool failed = notification->outcome == AccessOutcome::Failed;
      if (failed && !options_.publish_failed_loads) {
        retry(*operation, record);
      } else {
        if (failed) ++failed_publications_;
        recorder_.record(PublishEvent{*record.publishes, to_ref(record.destination), *operation});
        published_.insert(*record.publishes);
      }
    }
    if (record.abandoned) continue;  // Its holds were dropped on timeout.
    drop(record.source);
    drop(record.destination);
  }
}

void CacheHost::expire(const OperationId& operation, const std::string& reason) {
  auto& record = operations_.at(operation);
  recorder_.record(ExpireOperationEvent{operation, reason});
  if (options_.timeout == TimeoutPolicy::Reclaim && !record.notified && !record.abandoned) {
    record.abandoned = true;
    drop(record.source);
    drop(record.destination);
  }
}

BufferHandle CacheHost::reload(const OperationId& operation) {
  auto& original = operations_.at(operation);
  if (!original.publishes || !original.descriptor) {
    throw std::logic_error("reload: " + operation.value + " is not a load");
  }
  const auto staging = allocate();
  std::copy(original.payload.begin(), original.payload.end(), pool_.bytes(staging).begin());
  const auto cache = allocate();
  const auto state = *original.publishes;
  const auto descriptor = *original.descriptor;
  const auto abandoned = original.destination;
  const OperationId attempt(operation.value + "-retry");
  original.retried_as = attempt;
  load(attempt, staging, cache, state, descriptor);
  release(staging);
  release(abandoned);
  return cache;
}

std::size_t CacheHost::unacknowledged() const {
  return static_cast<std::size_t>(
      std::count_if(operations_.begin(), operations_.end(),
                    [](const auto& entry) { return !entry.second.notified; }));
}

void CacheHost::retry(const OperationId& operation, const OperationRecord& record) {
  // The failed load's staging buffer may already be retiring, so stage the
  // bytes again into a new buffer. The retry has its own identity.
  const auto staging = allocate();
  std::copy(record.payload.begin(), record.payload.end(), pool_.bytes(staging).begin());
  const OperationId attempt(operation.value + "-retry");
  OperationRecord next{staging, record.destination, record.publishes, false, record.payload};
  next.descriptor = record.descriptor;
  next.route = engine_.submit(attempt, RequestId("loader"), staging, record.destination);
  track(attempt, std::move(next));
  release(staging);
}

bool CacheHost::notified(const OperationId& operation) const {
  const auto found = operations_.find(operation);
  return found != operations_.end() && found->second.notified;
}

void CacheHost::hold(const BufferHandle& buffer) {
  const auto found = buffers_.find(buffer);
  if (found == buffers_.end()) throw std::logic_error("hold: buffer is not live");
  ++found->second.holds;
}

void CacheHost::drop(const BufferHandle& buffer) {
  const auto found = buffers_.find(buffer);
  if (found == buffers_.end()) return;  // Already reclaimed.
  auto& record = found->second;
  if (record.holds > 0) --record.holds;
  if (options_.reclaim == ReclaimPolicy::Deferred && record.released && record.holds == 0) {
    reclaim(buffer);
  }
}

void CacheHost::reclaim(const BufferHandle& buffer) {
  Hazard hazard{buffer, engine_.accessing(buffer), false};
  for (const auto& [request, held] : consumers_) {
    (void)request;
    if (std::find(held.begin(), held.end(), buffer) != held.end()) hazard.live_consumer = true;
  }
  if (hazard.pending_access || hazard.live_consumer) hazards_.push_back(hazard);
  pool_.reclaim(buffer);
  buffers_.erase(buffer);
}

namespace {

// The shared-prefix scenario. Two prefixes are staged and loaded into cache
// buffers; two requests share prefix-1 and each copies it out; request-1 is
// cancelled while its copy may still be running; prefix-1 is evicted while
// request-2 still uses it; a new staging buffer then takes whichever slot was
// freed last. Steps 3 and 7 wait for a notification. Between steps, any
// pending copy may complete and queued notifications may be processed, so the
// explored schedules cover every legal completion order without removing a
// synchronization edge the host relies on.
constexpr std::size_t kSlots = 8;
constexpr std::size_t kSlotBytes = 64;
constexpr std::byte kPrefix1Bytes{0x11};
constexpr std::byte kPrefix2Bytes{0x22};
constexpr std::byte kPrefix3Bytes{0x33};

const std::vector<const char*> kSharedPrefixSteps = {
    "stage and load prefix-1",                // 0
    "release staging-1",                      // 1
    "stage, load, and release prefix-2",      // 2
    "serve request-1",                        // 3: waits for prefix-1 to be published
    "serve request-2",                        // 4
    "cancel request-1",                       // 5
    "evict prefix-1 and stage prefix-3",      // 6
    "finish request-2",                       // 7: waits for send-2's notification
    "release remaining buffers",              // 8
};

// The reconnect scenario. load-1 is submitted in epoch 1 and may complete
// before or after the endpoint reconnects; load-2 then gets the same tag in
// epoch 2. request-1 reads prefix-2 once it is published. A host that
// matches notifications by tag alone can credit load-1's notification, late
// or redelivered, to load-2, publish prefix-2 before its copy ran, and
// reclaim load-2's staging buffer while the copy still reads it.
const std::vector<const char*> kReconnectSteps = {
    "stage, load, and release prefix-1",      // 0
    "reconnect peer-a",                       // 1
    "stage, load, and release prefix-2",      // 2
    "serve request-1",                        // 3: waits for prefix-2 to be published
    "finish request-1",                       // 4: waits for send-1's notification
    "release remaining buffers",              // 5
};

// The timeout scenario. load-1 is slow: it cannot complete before the host
// gives up on it. The host records the expiry and retries the load into a
// new cache buffer, so the two attempts never write the same buffer. The
// original copy keeps running and completes later. request-1 reads prefix-1
// once the retry is published.
const std::vector<const char*> kTimeoutSteps = {
    "stage, load, and release prefix-1",      // 0
    "time out load-1 and retry it",           // 1
    "serve request-1",                        // 2: waits for prefix-1 to be published
    "finish request-1",                       // 3: waits for send-1's notification
    "release remaining buffers",              // 4
};

const std::vector<const char*>& steps_of(ScenarioKind kind) {
  switch (kind) {
    case ScenarioKind::Reconnect: return kReconnectSteps;
    case ScenarioKind::TimeoutRetry: return kTimeoutSteps;
    case ScenarioKind::SharedPrefix: break;
  }
  return kSharedPrefixSteps;
}

EngineOptions engine_options(const ScenarioOptions& options) {
  auto engine = options.engine;
  if (options.kind != ScenarioKind::SharedPrefix && !engine.endpoint) {
    engine.endpoint = EndpointId("peer-a");
  }
  return engine;
}

SemanticDescriptor prefix_descriptor(const std::string& prefix) {
  return {NamespaceId("demo"), "model-r1", "async-copy", "layer-0", prefix,
          0, 16, "fp16", "row-major", "absolute"};
}

class Scenario {
 public:
  explicit Scenario(const ScenarioOptions& options)
      : kind_(options.kind),
        steps_(steps_of(options.kind)),
        recorder_(options.capture_capacity),
        pool_(recorder_, kSlots, kSlotBytes),
        engine_(recorder_, pool_, engine_options(options)),
        host_(recorder_, pool_, engine_, options.host) {}

  struct Option {
    enum class Kind { Step, Poll, Complete } kind;
    OperationId operation;
  };

  std::vector<Option> options() const {
    std::vector<Option> result;
    if (next_step_ < steps_.size() && ready(next_step_)) {
      result.push_back({Option::Kind::Step, {}});
    }
    if (engine_.has_notifications()) result.push_back({Option::Kind::Poll, {}});
    for (auto& operation : engine_.pending()) {
      // The slow load cannot finish before the host times it out.
      if (kind_ == ScenarioKind::TimeoutRetry && next_step_ <= 1 &&
          operation.value == "load-1") {
        continue;
      }
      result.push_back({Option::Kind::Complete, std::move(operation)});
    }
    return result;
  }

  void take(const Option& option) {
    switch (option.kind) {
      case Option::Kind::Step:
        schedule_.push_back(steps_[next_step_]);
        run_step(next_step_++);
        break;
      case Option::Kind::Poll:
        schedule_.push_back("process notifications");
        host_.poll();
        break;
      case Option::Kind::Complete:
        schedule_.push_back("complete " + option.operation.value);
        engine_.complete(option.operation);
        break;
    }
  }

  ScenarioRun finish() {
    ScenarioRun run;
    run.trace = recorder_.finish();
    run.capture = recorder_.stats();
    run.hazards = host_.hazards();
    run.failed_publications = host_.failed_publications();
    run.misattributions = host_.misattributions();
    run.unacknowledged = host_.unacknowledged();
    run.delivered_intact = delivered_intact_;
    run.schedule = std::move(schedule_);
    return run;
  }

 private:
  bool ready(std::size_t step) const {
    switch (kind_) {
      case ScenarioKind::SharedPrefix:
        if (step == 3) return host_.published(StateId("prefix-1"));
        if (step == 7) return host_.notified(OperationId("send-2"));
        return true;
      case ScenarioKind::Reconnect:
        if (step == 3) return host_.published(StateId("prefix-2"));
        if (step == 4) return host_.notified(OperationId("send-1"));
        return true;
      case ScenarioKind::TimeoutRetry:
        if (step == 2) return host_.published(StateId("prefix-1"));
        if (step == 3) return host_.notified(OperationId("send-1"));
        return true;
    }
    return true;
  }

  void run_step(std::size_t step) {
    switch (kind_) {
      case ScenarioKind::SharedPrefix: run_shared_prefix_step(step); break;
      case ScenarioKind::Reconnect: run_reconnect_step(step); break;
      case ScenarioKind::TimeoutRetry: run_timeout_step(step); break;
    }
  }

  // Stages `value` into a new buffer, loads it into a new cache buffer as
  // `state`, and releases the staging buffer.
  BufferHandle stage_and_load(const char* operation, const char* state, std::byte value) {
    const auto staging = host_.allocate();
    host_.fill(staging, value);
    const auto cache = host_.allocate();
    host_.load(OperationId(operation), staging, cache, StateId(state), prefix_descriptor(state));
    host_.release(staging);
    return cache;
  }

  // Serves request-1 from `state` in `cache` by copying it into a new output
  // buffer as send-1.
  void serve(const char* state, const BufferHandle& cache) {
    host_.consume(RequestId("request-1"), StateId(state), cache, prefix_descriptor(state));
    output1_ = host_.allocate();
    host_.send(OperationId("send-1"), RequestId("request-1"), cache, output1_);
  }

  void finish_request_1(std::byte expected) {
    const auto bytes = pool_.bytes(output1_);
    delivered_intact_ = std::all_of(bytes.begin(), bytes.end(),
                                    [&](std::byte b) { return b == expected; });
    host_.end_request(RequestId("request-1"));
  }

  void run_reconnect_step(std::size_t step) {
    switch (step) {
      case 0: cache1_ = stage_and_load("load-1", "prefix-1", kPrefix1Bytes); break;
      case 1: engine_.reconnect(); break;
      case 2: cache2_ = stage_and_load("load-2", "prefix-2", kPrefix2Bytes); break;
      case 3: serve("prefix-2", cache2_); break;
      case 4: finish_request_1(kPrefix2Bytes); break;
      case 5:
        for (const auto& buffer : {cache1_, cache2_, output1_}) host_.release(buffer);
        break;
    }
  }

  void run_timeout_step(std::size_t step) {
    switch (step) {
      case 0: cache1_ = stage_and_load("load-1", "prefix-1", kPrefix1Bytes); break;
      case 1:
        host_.expire(OperationId("load-1"), "timeout");
        cache2_ = host_.reload(OperationId("load-1"));
        break;
      case 2: serve("prefix-1", cache2_); break;
      case 3: finish_request_1(kPrefix1Bytes); break;
      case 4:
        for (const auto& buffer : {cache2_, output1_}) host_.release(buffer);
        break;
    }
  }

  void run_shared_prefix_step(std::size_t step) {
    const StateId prefix1("prefix-1");
    switch (step) {
      case 0:
        staging1_ = host_.allocate();
        host_.fill(staging1_, kPrefix1Bytes);
        cache1_ = host_.allocate();
        host_.load(OperationId("load-1"), staging1_, cache1_, prefix1,
                   prefix_descriptor("prefix-1"));
        break;
      case 1: host_.release(staging1_); break;
      case 2:
        staging2_ = host_.allocate();
        host_.fill(staging2_, kPrefix2Bytes);
        cache2_ = host_.allocate();
        host_.load(OperationId("load-2"), staging2_, cache2_, StateId("prefix-2"),
                   prefix_descriptor("prefix-2"));
        host_.release(staging2_);
        break;
      case 3:
        host_.consume(RequestId("request-1"), prefix1, cache1_, prefix_descriptor("prefix-1"));
        output1_ = host_.allocate();
        host_.send(OperationId("send-1"), RequestId("request-1"), cache1_, output1_);
        break;
      case 4:
        host_.consume(RequestId("request-2"), prefix1, cache1_, prefix_descriptor("prefix-1"));
        output2_ = host_.allocate();
        host_.send(OperationId("send-2"), RequestId("request-2"), cache1_, output2_);
        break;
      case 5: host_.end_request(RequestId("request-1")); break;
      case 6:
        host_.release(cache1_);
        staging3_ = host_.allocate();
        host_.fill(staging3_, kPrefix3Bytes);
        break;
      case 7: {
        const auto bytes = pool_.bytes(output2_);
        delivered_intact_ = std::all_of(bytes.begin(), bytes.end(),
                                        [](std::byte b) { return b == kPrefix1Bytes; });
        host_.end_request(RequestId("request-2"));
        break;
      }
      case 8:
        for (const auto& buffer : {output1_, output2_, cache2_, staging3_}) {
          host_.release(buffer);
        }
        break;
    }
  }

  ScenarioKind kind_;
  const std::vector<const char*>& steps_;
  TraceRecorder recorder_;
  BufferPool pool_;
  CopyEngine engine_;
  CacheHost host_;
  std::size_t next_step_ = 0;
  BufferHandle staging1_, cache1_, staging2_, cache2_, output1_, output2_, staging3_;
  bool delivered_intact_ = false;
  std::vector<std::string> schedule_;
};

// Runs one schedule. `taken` and `widths` receive the option chosen and the
// number of options at every point, for enumerating sibling schedules.
ScenarioRun execute(const ScenarioOptions& options, const std::vector<std::size_t>& choices,
                    std::vector<std::size_t>* taken, std::vector<std::size_t>* widths) {
  Scenario scenario(options);
  for (std::size_t point = 0;; ++point) {
    const auto available = scenario.options();
    if (available.empty()) break;
    const std::size_t choice = point < choices.size() ? choices[point] : 0;
    if (choice >= available.size()) {
      throw std::out_of_range("schedule choice " + std::to_string(choice) + " at point " +
                              std::to_string(point) + " has only " +
                              std::to_string(available.size()) + " options");
    }
    if (taken != nullptr) taken->push_back(choice);
    if (widths != nullptr) widths->push_back(available.size());
    scenario.take(available[choice]);
  }
  return scenario.finish();
}

}  // namespace

ScenarioRun run_scenario(const ScenarioOptions& options,
                         const std::vector<std::size_t>& choices) {
  return execute(options, choices, nullptr, nullptr);
}

std::size_t explore_scenario(const ScenarioOptions& options,
                             const std::function<void(const ScenarioRun&)>& visit) {
  std::vector<std::size_t> prefix;
  std::size_t runs = 0;
  for (;;) {
    std::vector<std::size_t> taken;
    std::vector<std::size_t> widths;
    visit(execute(options, prefix, &taken, &widths));
    ++runs;
    // Advance to the next schedule in depth-first order: bump the deepest
    // point that still has an untried option and take option 0 after it.
    std::size_t point = taken.size();
    while (point > 0 && taken[point - 1] + 1 >= widths[point - 1]) --point;
    if (point == 0) return runs;
    prefix.assign(taken.begin(), taken.begin() + static_cast<std::ptrdiff_t>(point - 1));
    prefix.push_back(taken[point - 1] + 1);
  }
}

}  // namespace stateguard::harness
