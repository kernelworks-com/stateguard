#include "cancellation.hpp"

namespace stateguard::harness {

CancellationRun run_cancellation(const CancellationOptions& options) {
  TraceRecorder recorder(options.capture_capacity);
  if (options.clock.now_ns) recorder.set_clock(options.clock);
  BufferPool pool(recorder, 3, options.slot_bytes);
  CopyEngine engine(recorder, pool);
  const SemanticDescriptor descriptor{NamespaceId("demo"), "model-r1", "cpu-cache",
                                      "layer-0", "prefix-1", 0, 16, "fp16",
                                      "row-major", "absolute"};
  const StateId state("state-1");
  const RequestId cancelled("request-1");
  const RequestId live("request-2");

  const auto staging = pool.acquire();
  const auto cache = pool.acquire();
  for (auto& byte : pool.bytes(staging)) byte = std::byte{0x5a};
  recorder.record(BindStateEvent{to_ref(cache), state, descriptor});
  engine.submit(OperationId("load-1"), cancelled, staging, cache);
  engine.complete(OperationId("load-1"));
  recorder.record(PublishEvent{state, to_ref(cache), OperationId("load-1")});
  pool.retire(staging);
  pool.reclaim(staging);

  recorder.record(ConsumeEvent{cancelled, state, to_ref(cache), descriptor});
  if (options.shared_live_consumer) {
    recorder.record(ConsumeEvent{live, state, to_ref(cache), descriptor});
  }
  const auto output = pool.acquire();
  engine.submit(OperationId("copy-1"), cancelled, cache, output);

  recorder.record(CancelSignalEvent{cancelled});
  if (options.scheduler_delay) options.scheduler_delay();
  recorder.record(CancelRequestEvent{cancelled});
  if (options.copy_delay) options.copy_delay();

  if (options.complete_copy) {
    engine.complete(OperationId("copy-1"));
    pool.retire(output);
    pool.reclaim(output);
    if (options.shared_live_consumer) recorder.record(FinishRequestEvent{live});
    pool.retire(cache);
    pool.reclaim(cache);
    // The pool reuses the cache slot, then the output slot.
    pool.acquire();
    pool.acquire();
  }
  // Without a completion the copy may still touch both buffers, so the host
  // keeps them.
  CancellationRun run;
  run.trace = recorder.finish();
  run.capture = recorder.stats();
  return run;
}

}  // namespace stateguard::harness
