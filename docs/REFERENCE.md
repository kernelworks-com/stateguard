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
double-completed operations, invalid lifecycle transitions, and incomplete
evidence. Retirement is idempotent. A cancellation releases a logical
consumer only; it does not release leases held by submitted asynchronous work.
Publication waits for every writer lease on that allocation generation to
complete. When a completion proof is supplied, it must identify a completed
operation that held the matching initialization write lease.

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

`--fault NAME` plants one violation in the history. It is placed where its
precondition holds, and the rest of the history stays legal. The tests require
the checker to report only that fault, at the planted event:

| Fault | Expected result |
|---|---|
| `premature_reclaim` | `violation` / `premature_reclaim` |
| `stale_generation` | `violation` / `stale_generation` |
| `incompatible_semantic_reuse` | `violation` / `incompatible_semantic_reuse` |
| `write_through_shared_immutable_state` | `violation` / `write_through_shared_immutable_state` |
| `double_completion` | `violation` / `double_completion` |
| `orphaned_completion` | `violation` / `orphaned_operation` |
| `publish_before_writer_completion` | `violation` / `invalid_state_transition` |
| `dropped_completion` | `inconclusive` / `incomplete_evidence` |
| `coverage_gap` | `inconclusive` / `incomplete_evidence` |
| `incomplete_end` | `inconclusive` / `incomplete_evidence` |

Generated histories are synthetic. They test the checker's model; they do not
show how any engine behaves or how often these failures occur in practice.

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
