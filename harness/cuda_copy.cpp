#include "cuda_copy.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace stateguard::harness::cuda {

namespace {

void cuda_call(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(what) + ": " + cudaGetErrorName(status) + ": " +
                             cudaGetErrorString(status));
  }
}

cudaStream_t as_stream(void* stream) { return static_cast<cudaStream_t>(stream); }

// Runs on a CUDA host thread in stream order; it must not call CUDA.
void CUDART_CB sleep_on_stream(void* milliseconds) {
  std::this_thread::sleep_for(*static_cast<const std::chrono::milliseconds*>(milliseconds));
}

}  // namespace

DeviceInfo device_info() {
  DeviceInfo info;
  cuda_call(cudaGetDevice(&info.device), "cudaGetDevice");
  cudaDeviceProp properties{};
  cuda_call(cudaGetDeviceProperties(&properties, info.device), "cudaGetDeviceProperties");
  info.name = properties.name;
  info.compute_major = properties.major;
  info.compute_minor = properties.minor;
  cuda_call(cudaRuntimeGetVersion(&info.runtime_version), "cudaRuntimeGetVersion");
  cuda_call(cudaDriverGetVersion(&info.driver_version), "cudaDriverGetVersion");
  return info;
}

DevicePool::DevicePool(TraceRecorder& recorder, std::size_t slots, std::size_t slot_bytes)
    : recorder_(recorder), slot_bytes_(slot_bytes), slots_(slots) {
  if (slots == 0 || slot_bytes == 0) {
    throw std::invalid_argument("device pool needs at least one nonempty slot");
  }
  int device = 0;
  cuda_call(cudaGetDevice(&device), "cudaGetDevice");
  domain_ = "cuda-" + std::to_string(device);
  try {
    for (auto& slot : slots_) cuda_call(cudaMalloc(&slot.memory, slot_bytes), "cudaMalloc");
  } catch (...) {
    for (auto& slot : slots_) cudaFree(slot.memory);
    throw;
  }
  for (std::size_t i = slots; i > 0; --i) free_.push_back(i - 1);
}

DevicePool::~DevicePool() {
  for (auto& slot : slots_) cudaFree(slot.memory);
}

DevicePool::Slot& DevicePool::checked(const BufferHandle& handle, const char* action) {
  if (handle.slot >= slots_.size() || slots_[handle.slot].generation != handle.generation ||
      slots_[handle.slot].state == SlotState::Free) {
    throw std::logic_error(std::string(action) + ": stale or free handle slot-" +
                           std::to_string(handle.slot) + " generation " +
                           std::to_string(handle.generation));
  }
  return slots_[handle.slot];
}

BufferHandle DevicePool::acquire() {
  if (free_.empty()) throw std::runtime_error("device pool exhausted");
  const auto index = free_.back();
  auto& slot = slots_[index];
  if (slot.generation == std::numeric_limits<Generation>::max()) {
    throw std::runtime_error("slot generation exhausted");
  }
  free_.pop_back();
  ++slot.generation;
  slot.state = SlotState::Live;
  const BufferHandle handle{index, slot.generation};
  const auto ref = to_ref(handle);
  recorder_.record(AllocateEvent{ref.allocation, ref.generation, slot_bytes_, domain_});
  return handle;
}

void DevicePool::retire(const BufferHandle& handle) {
  auto& slot = checked(handle, "retire");
  if (slot.state != SlotState::Live) {
    throw std::logic_error("retire: buffer is already retired");
  }
  recorder_.record(RetireEvent{to_ref(handle)});
  slot.state = SlotState::Retiring;
}

void DevicePool::reclaim(const BufferHandle& handle) {
  auto& slot = checked(handle, "reclaim");
  if (slot.state != SlotState::Retiring) {
    throw std::logic_error("reclaim: buffer was not retired");
  }
  recorder_.record(ReclaimEvent{to_ref(handle)});
  slot.state = SlotState::Free;
  free_.push_back(handle.slot);
}

void* DevicePool::address(const BufferHandle& handle) {
  return checked(handle, "address").memory;
}

struct CudaCopyEngine::Job {
  OperationId operation;
  cudaEvent_t done = nullptr;

  explicit Job(OperationId op) : operation(std::move(op)) {
    cuda_call(cudaEventCreateWithFlags(&done, cudaEventDisableTiming), "cudaEventCreate");
  }
  ~Job() { cudaEventDestroy(done); }
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
};

CudaCopyEngine::CudaCopyEngine(TraceRecorder& recorder, DevicePool& pool,
                               CudaEngineOptions options)
    : recorder_(recorder), pool_(pool), options_(options) {
  cudaStream_t stream = nullptr;
  cuda_call(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreate");
  stream_ = stream;
}

CudaCopyEngine::~CudaCopyEngine() {
  cudaStreamSynchronize(as_stream(stream_));
  jobs_.clear();
  cudaStreamDestroy(as_stream(stream_));
}

void CudaCopyEngine::submit(const OperationId& operation, const RequestId& request,
                            const BufferHandle& source, const BufferHandle& destination) {
  if (std::find(submitted_.begin(), submitted_.end(), operation) != submitted_.end()) {
    throw std::logic_error("operation identity reused: " + operation.value);
  }
  if (source.slot == destination.slot) {
    throw std::logic_error("copy source and destination share a slot");
  }
  const void* from = pool_.address(source);
  void* to = pool_.address(destination);
  auto job = std::make_unique<Job>(operation);
  // Recorded before the copy is enqueued, so the leases precede any access.
  recorder_.record(SubmitEvent{operation, request,
                               {Lease{to_ref(source), AccessKind::Read},
                                Lease{to_ref(destination), AccessKind::Write}},
                               {}});
  submitted_.push_back(operation);
  if (options_.start_delay.count() > 0) {
    cuda_call(cudaLaunchHostFunc(as_stream(stream_), sleep_on_stream, &options_.start_delay),
              "cudaLaunchHostFunc");
  }
  cuda_call(cudaMemcpyAsync(to, from, pool_.slot_bytes(), cudaMemcpyDeviceToDevice,
                            as_stream(stream_)),
            "cudaMemcpyAsync");
  cuda_call(cudaEventRecord(job->done, as_stream(stream_)), "cudaEventRecord");
  jobs_.push_back(std::move(job));
}

std::vector<OperationId> CudaCopyEngine::poll() { return collect(false); }

std::vector<OperationId> CudaCopyEngine::wait() { return collect(true); }

std::vector<OperationId> CudaCopyEngine::collect(bool block) {
  std::vector<OperationId> notifications;
  for (auto job = jobs_.begin(); job != jobs_.end();) {
    if (block) {
      cuda_call(cudaEventSynchronize((*job)->done), "cudaEventSynchronize");
    } else {
      const auto status = cudaEventQuery((*job)->done);
      if (status == cudaErrorNotReady) {
        ++job;
        continue;
      }
      cuda_call(status, "cudaEventQuery");
    }
    // The event completed, so the copy has stopped touching both buffers.
    recorder_.record(CompleteAccessEvent{(*job)->operation});
    notifications.push_back((*job)->operation);
    job = jobs_.erase(job);
  }
  return notifications;
}

bool CudaCopyEngine::pending(const OperationId& operation) const {
  return std::any_of(jobs_.begin(), jobs_.end(),
                     [&](const auto& job) { return job->operation == operation; });
}

namespace {

constexpr unsigned char kPrefix1Byte = 0x11;
constexpr unsigned char kPrefix2Byte = 0x33;

SemanticDescriptor prefix_descriptor(const std::string& prefix) {
  return {NamespaceId("demo"), "model-r1", "cuda-copy", "layer-0", prefix,
          0, 16, "fp16", "row-major", "absolute"};
}

// A second stream for staging writes. It does not wait for the copy stream.
class StagingStream {
 public:
  StagingStream() {
    cuda_call(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreate");
  }
  ~StagingStream() {
    cudaStreamSynchronize(stream_);
    cudaStreamDestroy(stream_);
  }
  StagingStream(const StagingStream&) = delete;
  StagingStream& operator=(const StagingStream&) = delete;
  cudaStream_t get() const { return stream_; }

 private:
  cudaStream_t stream_ = nullptr;
};

}  // namespace

GpuScenarioRun run_gpu_scenario(const GpuScenarioOptions& options) {
  TraceRecorder recorder(options.capture_capacity);
  DevicePool pool(recorder, 4, options.slot_bytes);
  StagingStream staging_stream;
  GpuScenarioRun run;
  std::vector<unsigned char> cache_bytes(options.slot_bytes);
  {
    CudaCopyEngine engine(recorder, pool, options.engine);
    const StateId prefix1("prefix-1");
    const OperationId load("load-1");

    const auto staging1 = pool.acquire();
    cuda_call(cudaMemset(pool.address(staging1), kPrefix1Byte, options.slot_bytes),
              "cudaMemset");
    cuda_call(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    const auto cache = pool.acquire();
    recorder.record(BindStateEvent{to_ref(cache), prefix1, prefix_descriptor("prefix-1")});
    engine.submit(load, RequestId("loader"), staging1, cache);

    // Release staging-1. The host keeps the copy's notification as its proof
    // that the source is no longer read; Immediate skips waiting for it.
    pool.retire(staging1);
    std::vector<OperationId> notified;
    if (options.reclaim == ReclaimPolicy::Deferred) notified = engine.wait();
    run.reclaimed_while_pending = engine.pending(load);
    pool.reclaim(staging1);

    // Stage prefix-2 into whichever slot was freed last.
    const auto staging2 = pool.acquire();
    cuda_call(cudaMemsetAsync(pool.address(staging2), kPrefix2Byte, options.slot_bytes,
                              staging_stream.get()),
              "cudaMemsetAsync");

    if (notified.empty()) notified = engine.wait();
    recorder.record(PublishEvent{prefix1, to_ref(cache), load});

    const RequestId request("request-1");
    recorder.record(ConsumeEvent{request, prefix1, to_ref(cache), prefix_descriptor("prefix-1")});
    cuda_call(cudaMemcpy(cache_bytes.data(), pool.address(cache), options.slot_bytes,
                         cudaMemcpyDeviceToHost),
              "cudaMemcpy");
    recorder.record(CancelRequestEvent{request});

    cuda_call(cudaStreamSynchronize(staging_stream.get()), "cudaStreamSynchronize");
    for (const auto& buffer : {cache, staging2}) {
      pool.retire(buffer);
      pool.reclaim(buffer);
    }
  }
  run.corrupt_bytes = static_cast<std::size_t>(
      std::count_if(cache_bytes.begin(), cache_bytes.end(),
                    [](unsigned char b) { return b != kPrefix1Byte; }));
  run.delivered_intact = run.corrupt_bytes == 0;
  run.trace = recorder.finish();
  run.capture = recorder.stats();
  return run;
}

}  // namespace stateguard::harness::cuda
