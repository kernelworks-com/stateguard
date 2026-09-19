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
`inconclusive` result. Definite invariant violations remain `violation` even
when the same trace also has incomplete coverage.

The checker currently reports stale generations, incompatible semantic reuse,
writes against published immutable state, premature reclaim, orphaned or
double-completed operations, invalid lifecycle transitions, and incomplete
evidence. Retirement is idempotent. A cancellation releases a logical
consumer only; it does not release leases held by submitted asynchronous work.
Publication waits for every writer lease on that allocation generation to
complete. When a completion proof is supplied, it must identify a completed
operation that held the matching initialization write lease.

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
