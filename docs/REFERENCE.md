# Technical reference

Start with the [quick start](../README.md) to run the examples.

## Workflow

Instrument allocation, transfer, retirement, and consumption boundaries; capture
an event trace; run the checker; inspect a diagnostic tied to the relevant state
and operation identifiers. Capture coverage and dropped events must be visible.

Traces use the line-oriented `.sgtrace` format, specified field by field in
[TRACE-FORMAT.md](TRACE-FORMAT.md).

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
double-completed operations, publication of a failed write, completions whose
transfer evidence belongs to another transfer, invalid lifecycle transitions,
and incomplete evidence. Retirement is idempotent. A cancellation releases a logical
consumer only; it does not release leases held by submitted asynchronous work.
Publication waits for every writer lease on that allocation generation to
complete. When a completion proof is supplied, it must identify a completed
operation that held the matching initialization write lease.

## Follow-on findings

When the checker rejects an event, it does not apply it, and it keeps checking
the rest of the trace. A later event can then fail only because the rejected
state change is missing. Such a finding is listed with `[follow-on]`; in the API
it has `Diagnostic::follow_on == true`. The flag never changes the status or the
exit code, and the first finding of a trace is never a follow-on, since its
cause is reported before it. `stateguard-check --primary-only` hides follow-on
findings in text and JSON output.

A finding is a follow-on only under one of these rules:

| Rejected earlier | Later finding marked `[follow-on]` |
|---|---|
| `reclaim` of a generation (a `premature_reclaim`, or a reclaim without `retire`), so the generation stays live | `allocate` of that allocation fails because it is already live |
| `allocate` of a new, higher generation because the allocation is still live | a reference to that generation fails as `stale_generation` or as an unknown allocation |
| `publish` of a generation that was not yet published, for any reason | `consume` of that generation fails because it is not published |
| `submit` of a new operation identity, for any reason | `orphaned_operation` for that identity: its completion, expiry, use as a dependency, or use as a publication proof |
| `complete` whose transfer evidence does not match the operation (`misattributed_completion` or malformed evidence), so the operation stays outstanding | a dependency on that operation that has not completed, a publication that precedes its completion (as the cited proof or as an outstanding writer), and the end-of-trace `incomplete_evidence` that it has no observed completion |

Other categories are never marked, because the rule would be a guess:
`premature_reclaim` (an operation whose completion was rejected may still be
accessing memory, so a later reclaim can be a real hazard),
`write_through_shared_immutable_state`, `incompatible_semantic_reuse`,
`double_completion`, `failed_write_publication`, `misattributed_completion`,
capture gaps, and findings after rejected events not in the table (for
example a rejected `bind_state`). A follow-on finding is still reported: the
same event might also fail for another reason that the missing state change
hides.

In the async-copy demo's immediate-reclaim trace, 14 of 16 findings are
follow-ons; the two primary findings are the two `premature_reclaim`s. In its
failed-publication trace, 2 of 3 findings are follow-ons: both consumptions of
the unpublished state.

## Failed completions

A completion can record that the operation's access failed:

```text
complete|load-1|failed
```

The outcome field and its compatibility rules are in the
[format specification](TRACE-FORMAT.md#complete-forms). In the API, the field is
`CompleteAccessEvent::outcome` (`AccessOutcome::Succeeded` or
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

## Retirement checks

Memory can be reused only after every access to it has ended. Several things
can look like the end of an access without proving it: a timeout, an expired
lease, a disconnected peer, a reconnected transport, or a notification that
belongs to a different transfer. These events let an adapter record them so
the checker can tell them apart from a real completion.

```text
connect|peer-a|1
submit|load-1|loader|r:staging-1:1;w:cache-1:1||peer-a:1:7
expire|load-1|timeout
complete|load-1|succeeded|peer-a:1:7
```

`connect` starts a new connection epoch of an endpoint, the optional route
field of `submit` names the operation's transfer (endpoint, epoch, and backend
tag), the optional evidence field of `complete` names the route the backend's
completion reported, and `expire` records that the host stopped waiting. The
[format specification](TRACE-FORMAT.md#events) gives their fields. Epochs must
increase, two operations cannot share a route, and a tag can repeat in a new
epoch.

The rules:

- A completion with evidence for a different route is `misattributed_completion`:
  a notification from an earlier epoch, or the completion of another attempt,
  such as the original load acknowledging its retry. The witness names the
  operation, its route, and the evidence. The completion is not accepted, so the
  operation's leases stay held and a later reclaim is premature.
- A completion with evidence matching its own route is accepted, even after a
  reconnect: a late notification for the old transfer does show that it ended.
  A second matching completion is a `double_completion`.
- A completion without evidence for an operation that has a route is accepted,
  but the trace is `inconclusive`, because nothing ties it to the transfer.
- `expire` and `connect` never release a lease. A reclaim while such an operation
  is outstanding is `premature_reclaim`, and the message names the expiry or the
  new epoch. A completion that arrives after the expiry still ends the access. An
  expired operation that never completes leaves the trace unresolved.

Fixtures: `routed_reconnect_clean.sgtrace` (clean),
`stale_epoch_notification.sgtrace` and `retry_acknowledges_other_attempt.sgtrace`
(`misattributed_completion`), `expired_lease_reclaim.sgtrace`
(`premature_reclaim`), and `missing_completion_evidence.sgtrace`
(`inconclusive`).

Limits: the checker compares the routes that the adapter records. If an adapter
copies the route from its own submission record instead of from the backend's
notification, a misattributed completion looks correct. Routes are opt-in per
operation; an operation without a route is checked as before. There is no event
yet for a backend confirming that an endpoint has stopped all access from an old
epoch, so each old operation needs its own completion. The generator does not
produce routed histories. Only the CPU [async-copy harness](#routed-transfers)
emits these events; no transport or engine adapter does yet.

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
- **Timestamps.** `set_clock()` (before the first event) stamps the trace:
  when the clock reading changes, `record()` adds a `time` event before the
  event, and the two are kept or dropped together. `finish()` stamps the end of
  capture. `steady_trace_clock()` reads `std::chrono::steady_clock`. The reading
  is when the adapter recorded the event, not when a device performed it.
- **Threads.** `TraceRecorder` is not thread-safe. Capture from one thread,
  serialize calls in an order that follows the rule above, or use
  `stateguard::LockedTraceRecorder`, which holds one mutex around `record()`,
  `set_clock()`, `finish()`, and `stats()` with the same signatures and
  exceptions. Its trace order is the lock acquisition order. To keep the
  record-before-effect rule across threads, record while holding the lock that
  publishes the effect (or call `record()` inside your own critical section for
  it); otherwise cross-thread order is only the recorder's lock order.
  Timestamps and dropped-event accounting stay exact: a time/event pair is kept
  or dropped together, and readings of a monotonic clock never decrease in the
  trace. Tested: 8 threads recording 1000 events each into a capacity of 3000,
  with and without a manual monotonic clock (recorded + dropped equals every
  call, one coverage gap, the checker parses the serialized trace and reports
  `inconclusive`), plus the pair-drop case; the suite also ran clean under
  ThreadSanitizer (clang, Debug). Instrumentation can change the timing of the
  code it observes.

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
After that finding, the checker reports [follow-on findings](#follow-on-findings):
the slot is reused while the checker still treats the old generation as live. In
the failed-publication run, the rejected publication is not recorded, so each
later consumption of that state is reported as a follow-on that consumes an
unpublished allocation. Run `stateguard-reduce` to get a witness for the first
failure.

### Routed transfers

With `EngineOptions::endpoint` set, the engine acts as a transport endpoint and
exercises the [retirement checks](#retirement-checks). It records `connect` for
epoch 1, gives each copy a route whose tag is counted from 1 in every epoch (so
tags repeat after `reconnect()`), and records no completions itself. Each
notification names its route and outcome; the host decides which operation it
belongs to and records `complete` with that route as evidence. With
`redeliver_on_reconnect`, a reconnect delivers the old epoch's notifications
again. `HostOptions::attribution` chooses how the host matches a notification:
by the whole route, by tag only, or by route but credited to the newest retry
of the operation. `HostOptions::timeout` chooses whether a timed-out operation
keeps its buffers until its completion arrives or drops them at once.

Two scenarios use routes (`ScenarioOptions::kind`):

- **Reconnect.** load-1 is submitted in epoch 1 and may complete before or after
  the endpoint reconnects. load-2 then gets the same tag in epoch 2, and a
  request reads prefix-2 once it is published.
- **Timeout and retry.** load-1 cannot complete before the host gives up on it.
  The host records `expire`, retries the load as `load-1-retry` into a new cache
  buffer, and releases the original buffers. The original copy keeps running and
  completes later.

Beyond unsafe reclaims, the harness's ground truth counts notifications the host
credited to an operation the engine did not run, and operations whose real
completion the host dropped as a repeat. The tests require, for every schedule:
a misattributed notification is a `violation` whose first finding is
`misattributed_completion`; a dropped completion without a misattribution is
`inconclusive`; otherwise the trace is `clean`.

| Configuration | Schedules | Result |
|---|---|---|
| Reconnect, host matches the whole route | 22 (24 with redelivery) | `clean` in every schedule, bytes intact |
| Reconnect, host matches the tag only | 55 (68 with redelivery) | `misattributed_completion` in 39 (52), naming load-2 and the epoch-1 evidence; `inconclusive` in 13 where load-1's completion was dropped; `clean` in 3 |
| Timeout, host waits for the late completion | 15 | `clean` in every schedule, bytes intact |
| Timeout, host reclaims on timeout | 15 | `premature_reclaim` in every schedule, citing the expiry; bytes intact in all 15 |
| Timeout, original completion credited to the retry | 26 | `misattributed_completion` in 13, naming load-1-retry; `inconclusive` in the other 13, where the retry was credited first and the original's completion was dropped |

Reclaiming on timeout never corrupted the delivered bytes here, because the
retry writes a new buffer, but the original copy still ran against reclaimed
memory. The checker reports the ordering either way. These scenarios have few
schedules because each has only two or three copies.

Limits: the harness runs on the CPU in one thread, and its schedules are the
completion and notification orders of one fixed scenario. It does not show that
CUDA, RDMA, NIXL, or any serving engine emits faithful events. Its cache host
still ends finished requests with `cancel`; the `finish` event described under
[Cancellation timeline](#cancellation-timeline) is not used there yet. A copy fails only when the test names it in `failing_operations`, and
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

Checked on 2026-11-04 at commit 9ee081c: Tesla T4 (compute 7.5), driver
580.82.07, CUDA 13.0, GCC 13.3 on Ubuntu 24.04, 27/27 CTest (5 `gpu` tests), no
compiler warnings. With 64 MiB copies and 20 runs per configuration, deferred
reclamation was `clean` in 20/20 runs and immediate reclamation was
`premature_reclaim` in 20/20 runs, with and without a 20 ms start delay. Without
the delay the immediate runs left the bytes intact in 20/20; with it every
immediate run left the cache corrupted, up to all 64 MiB in one run.

Earlier check on 2026-10-16: Tesla T4 (compute 7.5), driver 580.82.07, CUDA 13.0, GCC
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

## Cancellation timeline

`stateguard-timeline FILE` (and `build_timeline()` in `timeline.hpp`) reports what
happened to the memory a cancelled request touched, and when. It reads three
optional events; traces without them are unchanged and the checker accepts them:

```text
time|steady|1200345
cancel_signal|request-1
finish|request-2
```

`time` says the following events happened at that reading (nanoseconds) of the
named monotonic clock, `cancel_signal` that the process received a cancellation
signal, and `finish` that a request ended normally. Their fields are in the
[format specification](TRACE-FORMAT.md#events).

The existing `cancel` event is the scheduler's acknowledgment. The checker
treats `finish` like `cancel` (the request's consumer role ends), ignores
`cancel_signal` (a signal alone releases nothing), and ignores `time`.

For each request with a signal or an acknowledgment the timeline reports:

| Stage | Event that proves it |
|---|---|
| Signal receipt | `cancel_signal` |
| Scheduler acknowledgment | `cancel` |
| Last access | the request's latest `consume`, or `complete` of its own operations |
| Eligible for reuse | the allocation is retired, no lease on it is outstanding, and no consumer is live |
| Reclaim | `reclaim` (returned to the pool, not to the operating system) |
| Actual reuse | `allocate` of the next generation of the same allocation |

An allocation generation is *cancelled-only* while it is not reclaimed, at least
one request has consumed it or submitted an operation on it, and every such
request has been acknowledged as cancelled. A live or finished request keeps it
out, so a shared prefix still used by another request is not counted as waste.
Retention is integrated per allocation generation from that point to the
reclaim, so bytes shared by several cancelled requests are counted once.
Retention that is still open at the end is measured to the end of capture and
marked as a lower bound.

The timeline is `incomplete` (exit code 2) when the trace has a coverage gap, an
incomplete or missing end marker, no timestamps, a timestamp that decreases, a
second clock, a signal without an acknowledgment, or an operation without a
completion that touches memory of a cancelled request. Stages it cannot prove
are printed as `unknown`. Exit code 0 means complete; 3 is an input error. The
timeline does not check lifetimes, so run `stateguard-check` on the same trace.
`stateguard-reduce` drops timestamps from a witness, because they never change a
finding.

`stateguard-cancellation-demo` runs one scenario on the steady clock: request-1
loads a prefix, consumes it, and starts copying it out. Its cancellation is
signalled and acknowledged while the copy runs, and the copy completes after an
injected delay (`--delay-ms`, default 20). Excerpt for the delayed run:

```text
complete
clock steady
request request-1
  signal +9666 ns
  ack +9791 ns
  last_access +23228083 ns
  eligible +23235041 ns
  reuse +23236791 ns
  cancellation_to_reuse 23227125 ns
```

With a manual clock, the tests check that cancellation-to-reuse equals the
injected scheduler and copy delays exactly. On the steady clock, the measured
interval covered the 20 ms sleep in every run (20.2 to 25.0 ms in six runs; the
excess is sleep overshoot). A second request that shares the prefix and finishes
normally keeps the prefix out of the cancelled-only total. If the copy never
completes, the timeline is `incomplete` and the checker is `inconclusive`.

Capture cost, `stateguard-cancellation-demo --measure 1000` (macOS 26.3.1 arm64,
Apple clang 17, Release; three sessions of 1000 runs each after one warmup, one
thread, no injected delay; the scenario records 21 events):

| Mode | Median per run | Min | Max |
|---|---|---|---|
| Capture without timestamps | 2.17 to 2.21 us | 2.08 us | 9.38 us |
| Capture with timestamps (20 to 21 more events) | 2.58 us | 2.46 us | 46.4 us |
| Timestamps plus building the timeline | 6.42 to 6.50 us | 6.29 us | 62.9 us |

Limits: one process and one clock domain. The timeline orders events by trace
order and does not correlate clocks across processes or devices. A timestamp is
taken when the adapter records the event, so a device may finish earlier than
`complete` says. An acknowledged cancellation is not proof that an engine stopped
its work; only completions end accesses. GPU time of mixed batches is not split
between requests. The LMCache adapter and the CUDA demo do not record timestamps
yet.

## Machine-readable output

All four tools accept `--help` (usage, options, and exit codes on stdout, exit
0) and `--version` (`stateguard-<tool> 0.1.0`, exit 0). `stateguard-check`,
`stateguard-reduce`, and `stateguard-timeline` read the trace from standard
input when the file argument is `-`. Without the flags below, the text output
and the exit codes are unchanged.

`stateguard-check --quiet` prints the status line only. `stateguard-check
--primary-only` omits [follow-on findings](#follow-on-findings) from the text or
JSON list; the status and exit code are unchanged. `stateguard-check
--json` and `stateguard-timeline --json` print one JSON object on one line of
stdout and exit with the same code as the text mode. Usage errors and
unreadable or malformed traces print a message on stderr, no JSON, and exit 3.
Absent times are `null`. An allocation reference is
`{"allocation": "<id>", "generation": <n>}`; a request is its id string.

`stateguard-check --json`:

| Key | Type | Meaning |
|---|---|---|
| `status` | string | `clean`, `violation`, or `inconclusive` |
| `exit_code` | number | 0, 1, or 2, as the process exits |
| `capture_complete` | bool | the end marker says the capture was complete (`end\|complete`) |
| `diagnostics` | list | findings in trace order, each with `index` (event), `category`, `confirmed`, `follow_on`, `message`, and `witness` (list of strings) |

`stateguard-timeline --json` mirrors `TimelineReport` in
`include/stateguard/timeline.hpp`, with the same member names:

| Key | Type | Meaning |
|---|---|---|
| `status` | string | `complete` or `incomplete` |
| `clock` | string | the trace's clock, empty when untimed |
| `first_ns`, `last_ns` | number or null | first and last timestamp |
| `requests` | list | `request`, `signal_ns`, `ack_ns`, `last_access_ns`, `cancelled_only` (refs), `not_attributed` (refs), `eligible_ns`, `reuse_ns` |
| `allocations` | list | `allocation` (ref), `bytes`, `requests`, `last_access_ns`, `eligible_ns`, `reclaim_ns`, `reuse_ns`, `reclaimed_before_eligible`, `cancelled_only`, `cancelled_only_ns`, `cancelled_only_byte_seconds`, `retained_at_end` |
| `peak_cancelled_only_bytes` | number | deduplicated peak |
| `cancelled_only_byte_seconds` | number | deduplicated total |
| `incomplete` | list of strings | reasons the timeline is incomplete |

Times are absolute nanoseconds on the trace's clock, not relative to
`first_ns` as in the text output. Keys are stable within 0.x: a key is renamed
or removed only with a changelog entry. New keys may be added.

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

What has been tested ("supported" means these checks ran, not certification):

| Component | Tested on | Scope |
|---|---|---|
| Checker, capture, generator, reducer, timeline | macOS arm64 Apple Clang 17; Ubuntu 24.04 GCC 13 and Clang 18 | C++20, tests and synthetic fixtures |
| Install and CMake consumer | macOS arm64; Ubuntu 24.04 arm64 | Static and shared library, relocated prefix, uninstall |
| CUDA copy harness | One Tesla T4, driver 580.82.07, CUDA 13.0 | See [CUDA copy harness](#cuda-copy-harness); last checked at commit 9ee081c |
| LMCache static put hooks | Two pinned revisions, CPU mocked NIXL | See [LMCache static put adapter](#lmcache-static-put-adapter) |
| Windows, distributed clocks, RDMA, multi-GPU, production engines | Not tested | No support claim |

Known blind spots:

- A clean result does not show that an adapter captured every relevant event or
  modeled its backend's completion contract faithfully.
- Capture changes timing. `TraceRecorder` is not thread-safe.
  `LockedTraceRecorder` is tested only with synthetic events from 8 threads
  (including under ThreadSanitizer), not with a real multi-threaded engine, and
  its cross-thread order is the lock order unless the adapter records under the
  lock that publishes each effect.
- A rejected event can cause follow-on diagnostics. Only the cases in
  [Follow-on findings](#follow-on-findings) are marked; others are listed as
  primary findings.
- The CUDA engine throws on CUDA errors instead of recording a failed
  completion. The async-copy demo does not run the routed scenarios (the tests
  do), and the generator does not produce routed histories.
- There is no endpoint-quiesced event. The LMCache and CUDA producers do not
  record timestamps.
- Traces carry identifiers and structural metadata. Report issues with synthetic
  traces, not production captures.

## Relationship to other tools

Native memory checkers, deterministic fault testing, cache provenance systems,
and engine-specific regression suites cover related problems. Stateguard's research
focus is whether a portable semantic-state model can make lifetime failures easier
to detect and reproduce.

Relevant work includes the [TensorRT-LLM fault-injection proposal](https://github.com/NVIDIA/TensorRT-LLM/issues/18450)
and [KV-transfer lifecycle documentation](https://nvidia.github.io/TensorRT-LLM/latest/developer-guide/kv-transfer.html).
The project makes no claim to have invented inference testing or cache ownership.

## LMCache static put adapter

The [experimental adapter guide](LMCACHE.md) documents setup, hooks, and evidence
contracts for the two revisions around upstream PR #4658. This is optional Python
instrumentation tested with the upstream CPU mocks, not a real NIXL/GPU result.

Verified on macOS 26.3.1 arm64, Python 3.11.8, torch 2.14.1 CPU, with the
AppleClang 17 Release checker (2026-10-20):

| Check | Before fix (`919fe8d`) | Fixed (`217b1d8`) |
| --- | --- | --- |
| Independent upstream suite | 16 failed / 8 passed | 24 passed |
| Gated put, caller drops source reference | 5/5 `premature_reclaim` | 5/5 clean |
| Injected put failure, known-ended mock access | 5/5 inconclusive: unresolved `cleanup-*` operation | 5/5 clean rollback |
| Failure without quiescence proof | 1/1 inconclusive | 1/1 inconclusive |

The failure result deliberately reflects the visibility contract: the vulnerable
key stays in progress and is hidden by `contains()`, so the adapter does not invent
a failed publication. Its pending metadata operation reports missing cleanup.
The source case's first finding is `premature_reclaim`; subsequent findings can
cascade after rejected lifecycle events (see
[Follow-on findings](#follow-on-findings); the source case has not been rerun
since that marking was added). These reproduce the existing upstream
regression, not a newly discovered upstream issue.

Wall-clock microharness measurements, milliseconds, median [min, max], 20 measured
repetitions after one warmup per mode, modes run sequentially:

| Revision | Instrumentation off | Capture on | Capture + offline CLI checking |
| --- | --- | --- | --- |
| Before fix | 1.283 [1.157, 2.885] | 1.509 [1.322, 1.903] | 11.571 [10.138, 33.741] |
| Fixed | 1.169 [1.075, 2.847] | 1.339 [1.282, 1.993] | 12.242 [10.236, 16.548] |

Each sample creates the mocked backend and event-loop thread, performs one gated
32-byte source put and metadata lookup, and tears down the loop. Capture includes
hook setup, serialization, and restoration. Checking additionally writes a temporary
trace and launches the CLI; its cost includes process startup and file I/O. These
measurements are not serving throughput or latency estimates and make no production
overhead promise. Source contents and real transfer corruption are not measured.
Identity maps remain session-sized even when trace storage is bounded.

Dependency-free checks: 13 tests, including page-generation reuse, ref-count
transitions, restoration preserving pending progress, alias/pin rejection, schema
validation, overflow, and eight-thread capture of 1,000 transactions. Both Release
and ASan/UBSan checker executables accept the conformance traces. Engine concurrency
beyond the gated put/caller pattern remains unverified. CI runs the dependency-free
checks without installing LMCache; pinned engine reproduction is a separate command.
