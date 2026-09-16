# Stateguard

**Inference-state diagnostics from Kernelworks.**

Stateguard is a planned native tool for detecting invalid state lifetimes in
inference runtimes. Its focus is the logical correctness of cached state as it
is shared, transferred, cancelled, recycled, and consumed.

## Project status

**Pre-implementation.** This repository currently contains project documentation.
There is no installable library, supported engine integration, or published
performance result yet. The interfaces below describe intended behavior.

## The problem

An address can be valid while the state stored there is wrong for its consumer.
Asynchronous transfers and cache reuse make it difficult to determine whether a
page still represents the model, prefix, token range, and allocation generation a
request expects.

For example, an operation associated with a cancelled request might outlive the
allocation it was given. A useful diagnostic must connect the operation's lifetime
to the allocation's reuse, rather than merely checking whether the pointer exists.

## Intended capabilities

- Track logical state identity and allocation generations.
- Model valid sharing between requests without treating every shared page as an error.
- Record ownership and dependencies of asynchronous operations.
- Explain invalid reuse or consumption with a compact event trace.
- Reduce a failing trace into a smaller reproducible scenario.
- Support a portable CPU checker first, followed by explicit native integration points.

Stateguard is intended to run in the user's environment. Core diagnostics are
intended to work without a hosted service or mandatory telemetry.

## Intended workflow

Instrument allocation, transfer, retirement, and consumption boundaries; capture
an event trace; run the checker; inspect a diagnostic tied to the relevant state
and operation identifiers. Capture coverage and dropped events must be visible.

No command-line syntax or C++ API is stable yet. Build and installation instructions
will be added when a runnable implementation is available.

## Scope and limitations

Stateguard is planned as a diagnostic tool, not a proof that every inference result
is correct. Missing instrumentation limits what can be checked. A late completion
check cannot undo a DMA write that has already happened; preventing such writes
requires an appropriate ownership and transport protocol.

Initial work targets a small, documented event model and CPU reference harness.
CUDA and serving-engine support will be listed only after integration testing.

## Relationship to other tools

Native memory checkers, deterministic fault testing, cache provenance systems,
and engine-specific regression suites cover related problems. Stateguard's research
focus is whether a portable semantic-state model can make lifetime failures easier
to detect and reproduce.

Relevant work includes the [TensorRT-LLM fault-injection proposal](https://github.com/NVIDIA/TensorRT-LLM/issues/18450)
and [KV-transfer lifecycle documentation](https://nvidia.github.io/TensorRT-LLM/latest/developer-guide/kv-transfer.html).
The project makes no claim to have invented inference testing or cache ownership.

## Kernelworks

Stateguard is an independent Kernelworks project. It does not require Yieldpoint
or Branchforge.

## License

Apache License 2.0. See [LICENSE](LICENSE). Third-party dependencies and model
artifacts, when introduced, retain their own license terms.
