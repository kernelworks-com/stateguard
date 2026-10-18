# Technical reference

Start with the [quick start](../README.md) to run the examples.

## Workflow

Instrument allocation, transfer, retirement, and consumption boundaries; capture
an event trace; run the checker; inspect a diagnostic tied to the relevant state
and operation identifiers. Capture coverage and dropped events must be visible.

The current fixture format is a readable, line-oriented schema with a major
version header. It is suitable for synthetic fixtures and small captures; it is
not an ABI promise. Fields are separated by `|`, and identifiers and text fields
must not contain the reserved separators used by the format.

Build and run the checker with CMake and CTest:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build/stateguard-check tests/fixtures/safe_deferred_reclaim.sgtrace
build/stateguard-instrumented-demo
```

The public headers are in `include/stateguard/`. The core accepts typed state,
allocation, generation, request, and operation identities. A clean result
requires an explicit `end|complete` event. Dropped events, an incomplete end
marker, an unresolved operation, or a missing end marker produce an
`inconclusive` result.

## Findings after capture loss

A missing event can create a finding that is not real. For example, if a
completion is dropped, a later reclaim looks premature. The checker therefore
reports a finding as `violation` only when every event before it was captured:

- A finding before the first `coverage_gap` stays confirmed. The checker replays
  events in order, so such a finding depends only on captured events. The trace
  is a `violation` even though it also has a gap.
- A finding after a `coverage_gap` is listed with `[unconfirmed]` and makes the
  trace `inconclusive`. In the API, it has `Diagnostic::confirmed == false`.
- `end|incomplete` does not say where events were lost, so every finding in that
  trace is unconfirmed. Record a `coverage_gap` at the point of loss to keep
  earlier findings confirmed.
- A trace with no end marker lost only its tail. Findings in the observed
  prefix stay confirmed, and the trace is still reported as incomplete.

```text
inconclusive
3 incomplete_evidence: capture coverage gap: dropped completion records
...
8 premature_reclaim [unconfirmed]: allocation reclaimed while obligations remain (operation lease is still active)
```

That is `tests/fixtures/coverage_gap_unconfirmed.sgtrace`: a safe trace whose
completion record was lost. Every finding after the gap comes from the lost
event, so the result is inconclusive instead of a false violation.

The checker currently reports stale generations, incompatible semantic reuse,
writes against published immutable state, premature reclaim, orphaned or
double-completed operations, publication of a failed write, invalid lifecycle
transitions, and incomplete evidence. Retirement is idempotent. A cancellation releases a logical
consumer only; it does not release leases held by submitted asynchronous work.
Publication waits for every writer lease on that allocation generation to
complete. When a completion proof is supplied, it must identify a completed
operation that held the matching initialization write lease.

## Failed completions

A completion can record that the operation's access failed:

```text
complete|load-1|failed
```

The third field is optional. `succeeded` is also accepted, and a successful
completion is still written in the two-field form `complete|load-1`, so traces
without failures are unchanged. A reader that predates the field rejects the
line as malformed instead of reading the failure as a success. In the API, the
field is `CompleteAccessEvent::outcome` (`AccessOutcome::Succeeded` or
`AccessOutcome::Failed`).

A failed completion still ends the access: the operation no longer touches its
allocations, so its leases are released and a later reclaim is not premature.
What it does not do is leave valid data. The allocation generation it wrote is
treated as incomplete until a later writer to the same generation completes
successfully. The checker reports `failed_write_publication` when a `publish`:

- cites a failed operation as its completion proof, or
- names an allocation generation whose most recent completed write failed.

The witness names the allocation, generation, and failed operation. A retry
that writes the same generation and succeeds clears the failure, so the state
can then be published citing the retry
(`tests/fixtures/failed_write_retry.sgtrace` is `clean`;
`tests/fixtures/failed_write_publication.sgtrace` is a `violation`). The
checker only knows that a write failed if the adapter records the outcome; an
adapter that reports every completion as successful hides this failure.

## Trace reduction

`stateguard::reduce_trace` (and `stateguard-reduce`) shrinks a failing trace to a
witness for one target diagnostic. By default, the target is the first definite
violation. For an inconclusive trace, it is the first incomplete-evidence
diagnostic. `--category NAME` selects another category; the names are the ones
`stateguard-check` prints.

A candidate trace reproduces the target only when all of these hold:

- It has the same status (`violation` or `inconclusive`).
- It reports the target with the same category, message, witness lines, and
  confirmation. Deleting a coverage gap cannot turn an unconfirmed finding
  into a violation.
  Messages name identities and obligations, so a premature reclaim found by a
  live lease does not turn into one found by a live consumer.
- Every diagnostic it reports was also reported for the original trace.
  Deletions that create an unknown allocation, a publication before completion,
  or an operation with no observed completion are rejected.

The reduced trace is a subsequence of the original. Events are never reordered,
renamed, or edited, so the original causal order is preserved. Reduction runs
these passes:

1. Keep the causal slice: events connected to the target through shared
   allocation, state, operation, or request identities, plus coverage gaps and
   end markers.
2. Remove whole identity groups, such as one request, one operation, or one
   allocation generation's lifecycle.
3. Run delta debugging over the remaining events.
4. Remove single events.

Steps 2–4 repeat until none of them removes anything. The result is
**1-minimal**: removing any single remaining event, or every event that
mentions one identity, loses the failure. It is not guaranteed to be the
smallest possible witness. `--max-checks` limits the number of checker runs; when
the limit is reached, the summary says so and the best trace so far is returned.

The reduced trace goes to standard output (or `--output FILE`) and a summary
(target, event counts, kept original indices) to standard error. The command
exits with 0 after a reduction, 1 if the trace has no failure to reduce, and 3
for an input error. For example, `stateguard-reduce
tests/fixtures/premature_reclaim.sgtrace` reduces 6 events to 5.

A reduced trace is a checker witness under the current checker version. It does
not show that an engine reproduces the failure when replayed.

## Deterministic generation

`stateguard::generate_trace` (and `stateguard-generate`) produces a bounded,
legal lifecycle history from a seed and shape options: steps, allocations,
prefixes, and requests. Histories include allocation reuse across generations,
state shared by several allocations, copies between allocations, operation
dependencies, completions after cancellation, and duplicate publication and
retirement notifications. The generator uses its own SplitMix64 sequence instead
of standard library distributions, so a seed gives the same trace on every
platform.

```sh
./build/stateguard-generate --seed 7 --fault premature_reclaim --output generated.sgtrace
```

`--fault NAME` plants one violation in the history (`--list-faults` prints the
names). It is placed where its precondition holds, and the rest of the history
stays legal. The tests require the checker to report only that fault, at the
planted event:

| Fault | Expected result |
|---|---|
| `premature_reclaim` | `violation` / `premature_reclaim` |
| `stale_generation` | `violation` / `stale_generation` |
| `incompatible_semantic_reuse` | `violation` / `incompatible_semantic_reuse` |
| `write_through_shared_immutable_state` | `violation` / `write_through_shared_immutable_state` |
| `double_completion` | `violation` / `double_completion` |
| `orphaned_completion` | `violation` / `orphaned_operation` |
| `publish_before_writer_completion` | `violation` / `invalid_state_transition` |
| `failed_write_publication` | `violation` / `failed_write_publication` |
| `dropped_completion` | `inconclusive` / `incomplete_evidence` |
| `coverage_gap` | `inconclusive` / `incomplete_evidence` |
| `incomplete_end` | `inconclusive` / `incomplete_evidence` |

Generated histories are synthetic. They test the checker's model; they do not
show how any engine behaves or how often these failures occur in practice.

## Capturing events from native code

`stateguard::TraceRecorder` (in `capture.hpp`) collects events from instrumented
code and produces a trace for the checker:

- **Bounded memory.** The recorder reserves room for `capacity` events up front.
  The first event that does not fit, and every event after it, is dropped and
  counted. `finish()` then records a `coverage_gap` at that point and the end
  marker. Findings before the gap stay confirmed; the trace is never `clean`.
- **Order.** The trace order is the order of `record()` calls. Record an event
  before its effect becomes visible: record an access completion before the
  notification that lets the host release memory, and record a reclaim before
  the memory can be handed out again.
- **Reentrancy.** `record()` never calls back into instrumented code. Its storage
  uses the standard allocator, so it cannot instrument `operator new` or `malloc`
  themselves.
- **Threads.** The recorder is not thread-safe. Capture from one thread, or
  serialize calls in an order that follows the rule above. Instrumentation can
  change the timing of the code it observes.

An adapter that cannot observe part of the lifecycle should record its own
`coverage_gap` before the affected events. Findings after the gap are then
`[unconfirmed]` instead of being reported as violations.

## Async-copy harness

`harness/async_copy.hpp` is a small instrumented native program, built as the
`stateguard-async-copy` library for the demo and tests:

- `BufferPool` is a fixed-size slab allocator. Each slot is one allocation
  identity, reuse increments its generation, and freed slots are reused last
  in, first out. Host access goes through checked handles, so the pool rejects a
  stale generation itself. It records `allocate`, `retire`, and `reclaim`.
- `CopyEngine` copies between buffers asynchronously. It resolves both addresses
  when a copy is submitted, as a device would, and copies only when the caller
  completes the operation. Its completion contract: it copies, records
  `complete` with its outcome, and only then posts a notification. A recorded
  completion, failed or not, therefore proves that the copy no longer touches
  either buffer. Copies listed in `EngineOptions::failing_operations` write only
  the first half of the destination and complete as `failed`. Notifications
  can be delivered twice. An engine configured without completion reporting
  records a coverage gap instead.
- `CacheHost` loads staged bytes into cache buffers, publishes a state when its
  load is notified, lets requests consume and copy out shared state, and releases
  buffers. With the deferred policy, a released buffer is reclaimed once every
  copy that touches it has been notified and every consuming request has ended.
  The immediate policy reclaims at once. That is the induced bug. A host that
  counts every repeated notification as a release is a second induced bug.
  When a load fails, the host stages the bytes again into a new buffer and
  retries the load as `<operation>-retry` before publishing. With
  `HostOptions::publish_failed_loads`, it publishes the failed load instead.
  That is a third induced bug.

The scenario stages two prefixes and loads them. Two requests share prefix-1
and copy it out. One request is cancelled while its copy may still be running,
and prefix-1 is evicted while the other request still uses it. A new staging
buffer then takes the most recently freed slot. `explore_scenario` runs every
schedule: at each point, the host takes its next step, processes queued
notifications, or one pending copy completes. Host steps that wait for a
notification are never moved ahead of it, so no synchronization edge is
removed. Each configuration has 15,982 schedules, except the retried failed load,
whose extra copy gives it 41,550.

The harness also records which reclaims were unsafe from its own state: a copy
was still pending in the engine, or a request still consumed the buffer. It
also counts the failed loads it published. The checker sees only events. The
tests require the two to agree on every schedule:

| Configuration | Result |
|---|---|
| Deferred reclaim | `clean` in every schedule, and request-2's bytes are intact |
| Immediate reclaim | `violation` in every schedule; bytes corrupted in 5,594 |
| Repeated notifications counted as releases | `violation` in exactly the 13,278 schedules with an unsafe reclaim |
| Repeated notifications ignored | `clean` in every schedule |
| Capture buffer of 16 events | never `clean`; a `violation` only for an unsafe reclaim captured before the gap |
| Engine without completion reporting | `inconclusive` in every schedule |
| `load-1` fails, host retries | `clean` in all 41,550 schedules, and request-2's bytes are intact |
| `load-1` fails, host publishes it | `violation` in every schedule; the first finding is `failed_write_publication` for `load-1`, and request-2 does not receive the staged bytes |

For each reclaim violation, the first confirmed finding is `premature_reclaim` for the
first unsafe reclaim, with the matching reason (active lease or live consumer).
After that finding, the checker reports follow-on errors: the slot is reused while
the checker still treats the old generation as live. In the failed-publication
run, the rejected publication is not recorded, so each later consumption of that
state is also reported as consuming an unpublished allocation. Run
`stateguard-reduce` to get a witness for the first failure.

Limits: the harness runs on the CPU in one thread, and its schedules are the
completion and notification orders of one fixed scenario. It does not show that
CUDA, RDMA, NIXL, or any serving engine emits faithful events. Requests end with
`cancel`, because the event vocabulary has no separate event for a finished
request. A copy fails only when the test names it in `failing_operations`, and
it fails the same way each time: half the bytes are written, then the failure is
reported.

## CUDA copy harness

`harness/cuda_copy.hpp` is built only with `-DSTATEGUARD_ENABLE_CUDA=ON`. It calls
the CUDA runtime API from ordinary C++, so it needs the toolkit but not a CUDA
compiler, and the core library never includes CUDA headers.

- `DevicePool` holds `cudaMalloc` slots with the same identities, generation
  increments, last-in-first-out reuse, and stale-handle checks as `BufferPool`.
  Its allocations are recorded in domain `cuda-<device>`.
- `CudaCopyEngine` enqueues `cudaMemcpyAsync` on its own stream and records a CUDA
  event after each copy. Its completion contract: `complete` is recorded only
  after `cudaEventQuery` or `cudaEventSynchronize` reports that event complete,
  and only then is the host notified. Submission or a host timeout is never
  treated as completion.
- `run_gpu_scenario` loads a staged prefix into a cache buffer, releases the
  staging buffer, and restages the freed slot with `cudaMemsetAsync` on a second
  stream that does not wait for the copy. Deferred reclamation waits for the
  copy's completion; immediate reclamation does not. An optional host function
  delays the copy's start so that it reliably overlaps the restaging.

`stateguard-cuda-copy-demo` repeats each configuration and requires `clean` for
deferred and `premature_reclaim` for immediate reclamation on every run. Whether
the device delivered corrupt bytes is reported but not required. CTest runs it and
checks its traces under the `gpu` label; those tests need a CUDA device.

Checked on 2026-10-16: Tesla T4 (compute 7.5), driver 580.82.07, CUDA 13.0, GCC
13.3 on Ubuntu 24.04, 17/17 CTest. With 64 MiB copies and 20 runs per
configuration, the verdicts matched on every run. Without a start delay the copy
finished before the restaging write in all 20 immediate runs, so the bytes stayed
intact. With a 20 ms delay every immediate run corrupted all 64 MiB. The checker
reported the unsafe reclaim in both cases.

Limits: this is one GPU, driver, and toolkit on a shared cloud machine. The
harness emits its own events, so the result does not show that another CUDA
program, RDMA, NIXL, or a serving engine emits faithful events. The CUDA engine
records only successful completions; a CUDA error stops the demo with an
exception instead of being recorded as a failed completion.

## Scope and limitations

Stateguard is a diagnostic tool, not a proof that every inference result
is correct. Missing instrumentation limits what can be checked. A late completion
check cannot undo a DMA write that has already happened; preventing such writes
requires an appropriate ownership and transport protocol.

The event model treats semantic identity as a descriptor containing namespace,
model revision, adapter/component, prefix and token interval,
representation/layout, and position metadata. Generation numbers are tracked
separately from logical state identity and from physical allocation identifiers.
The checker explains observed event relationships; it does not prevent a DMA or
other backend write, and a CPU trace does not establish backend event
conformance.

## Relationship to other tools

Native memory checkers, deterministic fault testing, cache provenance systems,
and engine-specific regression suites cover related problems. Stateguard's research
focus is whether a portable semantic-state model can make lifetime failures easier
to detect and reproduce.

Relevant work includes the [TensorRT-LLM fault-injection proposal](https://github.com/NVIDIA/TensorRT-LLM/issues/18450)
and [KV-transfer lifecycle documentation](https://nvidia.github.io/TensorRT-LLM/latest/developer-guide/kv-transfer.html).
The project makes no claim to have invented inference testing or cache ownership.
