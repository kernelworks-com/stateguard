#pragma once

// An optional CUDA variant of the async-copy harness, built only with
// -DSTATEGUARD_ENABLE_CUDA=ON. It allocates device buffers, copies between
// them with cudaMemcpyAsync, and records Stateguard events around the copies.
// This header does not include CUDA headers; only cuda_copy.cpp does.
//
// A run on one GPU is evidence for that GPU, driver, and toolkit only. It says
// nothing about RDMA, NIXL, multi-GPU transfers, or serving engines.

#include "async_copy.hpp"
#include "stateguard/capture.hpp"
#include "stateguard/types.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace stateguard::harness::cuda {

struct DeviceInfo {
  int device = 0;
  std::string name;
  int compute_major = 0;
  int compute_minor = 0;
  // CUDA versions encoded as 1000 * major + 10 * minor.
  int runtime_version = 0;
  int driver_version = 0;
};

// Describes the current device. Throws std::runtime_error when CUDA reports an
// error, including when no device is available.
DeviceInfo device_info();

// Device memory split into fixed-size slots, with the same identities and
// checks as BufferPool: each slot is "slot-<index>", reuse increments its
// generation, free slots are reused last in, first out, and stale handles
// throw std::logic_error without recording anything. Every slot is allocated
// with cudaMalloc up front and freed when the pool is destroyed.
class DevicePool {
 public:
  DevicePool(TraceRecorder& recorder, std::size_t slots, std::size_t slot_bytes);
  ~DevicePool();
  DevicePool(const DevicePool&) = delete;
  DevicePool& operator=(const DevicePool&) = delete;

  // Records allocate in domain "cuda-<device>".
  BufferHandle acquire();
  void retire(const BufferHandle& handle);
  // Records reclaim, then makes the slot available for reuse. The pool cannot
  // see copies still running on the device; that is what the checker judges.
  void reclaim(const BufferHandle& handle);

  // The slot's device address. It is checked only when taken; afterwards it
  // aliases whichever generation owns the slot.
  void* address(const BufferHandle& handle);
  std::size_t slot_bytes() const { return slot_bytes_; }

 private:
  enum class SlotState { Free, Live, Retiring };
  struct Slot {
    void* memory = nullptr;
    Generation generation = 0;
    SlotState state = SlotState::Free;
  };

  Slot& checked(const BufferHandle& handle, const char* action);

  TraceRecorder& recorder_;
  std::size_t slot_bytes_;
  std::string domain_;
  std::vector<Slot> slots_;
  std::vector<std::size_t> free_;
};

struct CudaEngineOptions {
  // Before each copy, block the copy stream for this long with a host
  // function. This delays the copy without changing what it touches, so an
  // early reclaim reliably overlaps it. Zero leaves timing to the device.
  std::chrono::milliseconds start_delay{0};
};

// Device-to-device copies on one CUDA stream.
//
// Completion contract: submit() records the operation with a read lease on the
// source and a write lease on the destination, enqueues cudaMemcpyAsync on the
// copy stream, and records a CUDA event on that stream after the copy. poll()
// and wait() record `complete` for an operation only after cudaEventQuery or
// cudaEventSynchronize reports its event complete, which means the device has
// finished the copy and stopped touching both buffers. Only then is the
// notification returned to the host. Submission alone, or a host timeout, is
// not treated as completion.
class CudaCopyEngine {
 public:
  CudaCopyEngine(TraceRecorder& recorder, DevicePool& pool, CudaEngineOptions options = {});
  // Waits for outstanding copies so that no device work outlives the buffers.
  ~CudaCopyEngine();
  CudaCopyEngine(const CudaCopyEngine&) = delete;
  CudaCopyEngine& operator=(const CudaCopyEngine&) = delete;

  // Throws std::logic_error for a reused operation identity, a stale handle,
  // or a source and destination in one slot.
  void submit(const OperationId& operation, const RequestId& request,
              const BufferHandle& source, const BufferHandle& destination);
  // Records completion of every copy whose event has completed and returns
  // their notifications in submission order. Does not block.
  std::vector<OperationId> poll();
  // Blocks until every pending copy has completed, then behaves like poll().
  std::vector<OperationId> wait();

  bool pending(const OperationId& operation) const;

 private:
  struct Job;

  std::vector<OperationId> collect(bool block);

  TraceRecorder& recorder_;
  DevicePool& pool_;
  CudaEngineOptions options_;
  void* stream_ = nullptr;  // cudaStream_t
  std::vector<std::unique_ptr<Job>> jobs_;
  std::vector<OperationId> submitted_;
};

struct GpuScenarioOptions {
  ReclaimPolicy reclaim = ReclaimPolicy::Deferred;
  CudaEngineOptions engine;
  std::size_t slot_bytes = std::size_t{64} << 20;
  std::size_t capture_capacity = 256;
};

struct GpuScenarioRun {
  Trace trace;
  CaptureStats capture;
  // Whether the copy was still pending, by the engine's own record, when the
  // staging buffer was reclaimed. The checker never sees this.
  bool reclaimed_while_pending = false;
  // Whether the cache buffer read back from the device held the staged bytes.
  bool delivered_intact = false;
  std::size_t corrupt_bytes = 0;
};

// Loads a staged prefix into a cache buffer with one device copy, releases the
// staging buffer, and stages the next prefix into the slot that was freed with
// cudaMemsetAsync on a second stream that does not wait for the copy. With
// Deferred reclamation the staging buffer is reclaimed only after the copy's
// completion is observed. With Immediate it is reclaimed at once, and the new
// stage can overwrite the source while the copy reads it. A request then
// consumes the published prefix and the cache bytes are read back and
// compared with what was staged.
GpuScenarioRun run_gpu_scenario(const GpuScenarioOptions& options);

}  // namespace stateguard::harness::cuda
