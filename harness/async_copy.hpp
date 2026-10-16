#pragma once

// A small native allocator and asynchronous copy harness instrumented with
// Stateguard events. It runs on the CPU with a completion queue the caller
// controls, so every legal completion order of a scenario can be replayed
// deterministically. It is a demonstration of instrumentation, not an adapter
// for a real transport: CPU results say nothing about CUDA, RDMA, or NIXL.

#include "stateguard/capture.hpp"
#include "stateguard/types.hpp"

#include <compare>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace stateguard::harness {

// A checked handle to one generation of a pool slot.
struct BufferHandle {
  std::size_t slot = 0;
  Generation generation = 0;

  auto operator<=>(const BufferHandle&) const = default;
};

// The allocation identity recorded for a handle: "slot-<index>" plus its
// generation. The storage address is never part of the identity.
AllocationRef to_ref(const BufferHandle& handle);

// Fixed-size slab allocator. Each slot is one allocation identity, and reuse
// increments the slot's generation. Free slots are reused last in, first out,
// so a slot reclaimed too early is handed out again at once.
//
// The pool checks the handles the host gives it: touching, retiring, or
// reclaiming a stale generation, or reclaiming without retiring first, throws
// std::logic_error and records nothing. The pool cannot see asynchronous
// accesses or consumers; those obligations are what the checker evaluates.
class BufferPool {
 public:
  BufferPool(TraceRecorder& recorder, std::size_t slots, std::size_t slot_bytes);

  // Records allocate. Throws std::runtime_error when every slot is in use.
  BufferHandle acquire();
  // Records retire. The handle must be live and not yet retired.
  void retire(const BufferHandle& handle);
  // Records reclaim, then makes the slot available for reuse.
  void reclaim(const BufferHandle& handle);

  // Checked host access to a live or retiring generation.
  std::span<std::byte> bytes(const BufferHandle& handle);
  // The slot's storage, as a device would address it. It is checked only when
  // taken; afterwards it stays dereferenceable and aliases whichever
  // generation owns the slot. CopyEngine resolves addresses at submission.
  std::byte* address(const BufferHandle& handle);

  std::size_t slot_bytes() const { return slot_bytes_; }

 private:
  enum class SlotState { Free, Live, Retiring };
  struct Slot {
    std::vector<std::byte> storage;
    Generation generation = 0;
    SlotState state = SlotState::Free;
  };

  Slot& checked(const BufferHandle& handle, const char* action);

  TraceRecorder& recorder_;
  std::size_t slot_bytes_;
  std::vector<Slot> slots_;
  std::vector<std::size_t> free_;  // Top of the stack is reused next.
};

struct EngineOptions {
  // Whether the engine records access completion. An engine that cannot
  // observe it records a coverage gap when constructed instead: a notification
  // alone does not prove that the copy stopped touching memory.
  bool reports_completion = true;
  // Post every notification twice, as a transport that redelivers can.
  bool duplicate_notifications = false;
};

// Asynchronous buffer-to-buffer copies with a caller-controlled completion
// queue. submit() records the operation with a read lease on the source and a
// write lease on the destination and resolves both addresses; nothing is
// copied until complete() runs the copy.
//
// Completion contract: complete() performs the copy, then records `complete`
// for the operation, then posts the notification. A recorded completion
// therefore proves that the engine has finished touching both buffers. The
// notification is only a message to the host and may be duplicated.
class CopyEngine {
 public:
  CopyEngine(TraceRecorder& recorder, BufferPool& pool, EngineOptions options = {});

  // Throws std::logic_error for a reused operation identity or a stale handle.
  void submit(const OperationId& operation, const RequestId& request,
              const BufferHandle& source, const BufferHandle& destination);
  // Throws std::logic_error when the operation is not pending.
  void complete(const OperationId& operation);
  // The next notification, if any.
  std::optional<OperationId> poll();

  // Pending operations in submission order.
  std::vector<OperationId> pending() const;
  bool has_notifications() const { return !notifications_.empty(); }
  // Whether a pending copy can still read or write this generation.
  bool accessing(const BufferHandle& handle) const;

 private:
  struct Job {
    OperationId operation;
    BufferHandle source;
    BufferHandle destination;
    const std::byte* from = nullptr;
    std::byte* to = nullptr;
  };

  TraceRecorder& recorder_;
  BufferPool& pool_;
  EngineOptions options_;
  std::vector<Job> jobs_;
  std::set<OperationId> submitted_;
  std::deque<OperationId> notifications_;
};

enum class ReclaimPolicy {
  // Reclaim a released buffer only after every copy that touches it has been
  // notified and every request consuming it has ended.
  Deferred,
  // Reclaim as soon as the host releases a buffer. This is the induced bug.
  Immediate,
};

struct HostOptions {
  ReclaimPolicy reclaim = ReclaimPolicy::Deferred;
  // Ignore repeated notifications for one operation. When false, every
  // notification drops the operation's holds again. That is the induced bug.
  bool idempotent_notifications = true;
};

// A reclaim that was unsafe according to the harness's own state, which the
// checker never sees: the engine still had a copy that could touch the
// buffer, or a request was still consuming it.
struct Hazard {
  BufferHandle buffer;
  bool pending_access = false;
  bool live_consumer = false;
};

// A prefix cache host. It loads staged bytes into cache buffers, publishes a
// state once its load is notified, lets requests consume and copy out shared
// state, and releases buffers under the configured reclaim policy.
class CacheHost {
 public:
  CacheHost(TraceRecorder& recorder, BufferPool& pool, CopyEngine& engine,
            HostOptions options);

  BufferHandle allocate();
  void fill(const BufferHandle& buffer, std::byte value);
  // Binds the state to `cache` and copies `staging` into it as request
  // "loader". The state is published when the load's notification is
  // processed, unless `cache` has been released by then.
  void load(const OperationId& operation, const BufferHandle& staging,
            const BufferHandle& cache, const StateId& state,
            const SemanticDescriptor& descriptor);
  // Throws std::logic_error unless the state has been published.
  void consume(const RequestId& request, const StateId& state,
               const BufferHandle& cache, const SemanticDescriptor& descriptor);
  void send(const OperationId& operation, const RequestId& request,
            const BufferHandle& source, const BufferHandle& destination);
  // Records cancel_request, the event vocabulary's release of a request's
  // consumer role. It is used for cancelled and finished requests alike and
  // does not release the request's submitted copies.
  void end_request(const RequestId& request);
  void release(const BufferHandle& buffer);
  // Processes every queued notification.
  void poll();

  bool published(const StateId& state) const { return published_.contains(state); }
  bool notified(const OperationId& operation) const;
  const std::vector<Hazard>& hazards() const { return hazards_; }

 private:
  struct BufferRecord {
    std::size_t holds = 0;  // Unnotified copies touching it, plus consumers.
    bool released = false;
  };
  struct OperationRecord {
    BufferHandle source;
    BufferHandle destination;
    std::optional<StateId> publishes;
    bool notified = false;
  };

  void hold(const BufferHandle& buffer);
  void drop(const BufferHandle& buffer);
  void reclaim(const BufferHandle& buffer);

  TraceRecorder& recorder_;
  BufferPool& pool_;
  CopyEngine& engine_;
  HostOptions options_;
  std::map<BufferHandle, BufferRecord> buffers_;
  std::map<OperationId, OperationRecord> operations_;
  std::map<RequestId, std::vector<BufferHandle>> consumers_;
  std::set<StateId> published_;
  std::vector<Hazard> hazards_;
};

struct ScenarioOptions {
  HostOptions host;
  EngineOptions engine;
  std::size_t capture_capacity = 1024;
};

struct ScenarioRun {
  Trace trace;
  CaptureStats capture;
  std::vector<Hazard> hazards;
  // Whether request-2 received the bytes that were staged for prefix-1.
  bool delivered_intact = false;
  // The actions in the order they ran, for reporting a failing schedule.
  std::vector<std::string> schedule;
};

// Runs the shared-prefix scenario described in async_copy.cpp. At each point
// the options are, in order: the host's next step (when what it waits for has
// been notified), processing queued notifications (when there are any), and
// completing each pending copy in submission order. choices[i] selects the
// option at point i; when the choices run out, option 0 is taken. Throws
// std::out_of_range for a choice with no matching option.
ScenarioRun run_scenario(const ScenarioOptions& options,
                         const std::vector<std::size_t>& choices = {});

// Runs every schedule of the scenario once, depth first, and returns how many
// there were. The order of visits is deterministic.
std::size_t explore_scenario(const ScenarioOptions& options,
                             const std::function<void(const ScenarioRun&)>& visit);

}  // namespace stateguard::harness
