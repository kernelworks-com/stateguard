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

namespace {

std::optional<std::uint64_t> interval(std::optional<std::uint64_t> start,
                                      std::optional<std::uint64_t> end) {
  if (!start || !end || *end < *start) return std::nullopt;
  return *end - *start;
}

}  // namespace

CancellationSummary summarize_cancellation(const TimelineReport& report) {
  CancellationSummary summary;
  summary.status = report.status;
  summary.cancelled_requests = report.requests.size();
  if (report.requests.empty()) return summary;
  const auto& r = report.requests.front();
  summary.cancelled_only = r.cancelled_only.size();
  summary.signal_to_ack_ns = interval(r.signal_ns, r.ack_ns);
  const auto start = r.signal_ns ? r.signal_ns : r.ack_ns;
  summary.cancellation_to_eligible_ns = interval(start, r.eligible_ns);
  summary.cancellation_to_reuse_ns = interval(start, r.reuse_ns);
  return summary;
}

std::string to_string(const CancellationSummary& summary) {
  std::string out = to_string(summary.status);
  if (summary.cancelled_requests == 0) return out + ", no cancelled request";
  out += ", " + std::to_string(summary.cancelled_only) + " cancelled-only allocation" +
         (summary.cancelled_only == 1 ? "" : "s");
  const auto add = [&out](const char* name, const std::optional<std::uint64_t>& ns) {
    if (ns) out += std::string(", ") + name + ' ' + std::to_string(*ns) + " ns";
  };
  add("signal_to_ack", summary.signal_to_ack_ns);
  add("cancellation_to_eligible", summary.cancellation_to_eligible_ns);
  add("cancellation_to_reuse", summary.cancellation_to_reuse_ns);
  if (!summary.cancellation_to_reuse_ns) out += ", no cancellation_to_reuse";
  return out;
}

}  // namespace stateguard::harness
