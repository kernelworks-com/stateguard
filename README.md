# Stateguard

**Find when cached inference state is reused before its users are finished.**

A memory address can still exist while the state stored there belongs to a
different request or allocation generation. Stateguard checks recorded lifecycle
events and names the operation or allocation that broke a lifetime rule.

It is an experimental C++20 library and CLI. It runs on the CPU; an optional
CUDA harness has been checked on one NVIDIA T4. The LMCache adapter is
experimental and checked with CPU mocks at two pinned revisions; other engines
need their own event producer.

## Quick start

You need a C++20 compiler and CMake 3.20+. [Build, install, and uninstall](docs/BUILDING.md)
covers installing into a prefix; [scope and limitations](docs/REFERENCE.md#scope-and-limitations)
lists what has been tested.

```sh
git clone https://github.com/kernelworks-com/stateguard.git
cd stateguard
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
./build/stateguard-check tests/fixtures/safe_deferred_reclaim.sgtrace
./build/stateguard-check tests/fixtures/premature_reclaim.sgtrace
```

The first trace prints `clean`. The second starts with:

```text
violation
4 premature_reclaim: allocation reclaimed while obligations remain (operation lease is still active)
  allocation=buffer-1
  generation=1
```

| Result | Meaning | Exit code |
|---|---|---|
| `clean` | No modeled violation in a trace with complete evidence. | 0 |
| `violation` | An event breaks a checked lifetime or identity rule. | 1 |
| `inconclusive` | Events or completion evidence are missing. Findings after lost events are marked `[unconfirmed]`. | 2 |
| Input error | The file or trace format could not be accepted. | 3 |

A clean result covers only the supplied events and modeled rules.

## What it checks

- Reuse or consumption with a stale allocation generation or mismatched state.
- Reclaiming storage while a consumer or operation still holds it.
- Writing to published immutable state, publishing before writers finish, or
  publishing the result of a failed write.
- Invalid lifecycle transitions, orphaned operations, and duplicate completions.
- A completion acknowledged with evidence from another attempt or an earlier
  connection, and reclaims after a timeout that no completion confirmed.
- Missing completion evidence and gaps in capture coverage.

Shared state is allowed. Cancelling a request does not release an operation's
access to memory.

## Tools

Optional: [LMCache static-put adapter](docs/LMCACHE.md), with CPU mocked regression checks at two pinned revisions.

- `stateguard-check FILE` checks a trace.
- `stateguard-reduce FILE` shrinks a failing trace to a small witness that
  reproduces the same diagnostic. It only deletes events, never reorders them.
- `stateguard-timeline FILE` reports, for each cancelled request in a
  timestamped trace, signal, acknowledgment, last access, reuse eligibility, and
  actual reuse, plus memory retained only for cancelled requests
  ([details](docs/REFERENCE.md#cancellation-timeline)). Try
  `tests/fixtures/cancellation_timeline.sgtrace`: it prints `complete` and
  `cancellation_to_reuse 5500 ns`.
- `stateguard-generate --seed N [--fault NAME]` writes a deterministic synthetic
  history, optionally with one planted fault, for testing your own tooling.

Every tool accepts `--help`, `--version`, and `-` for standard input; `stateguard-check --json` and `stateguard-timeline --json` print one JSON object ([keys](docs/REFERENCE.md#machine-readable-output)).

`scripts/explain_trace.py` asks an OpenAI-compatible chat endpoint to explain the first finding; trace contents leave your machine and the reply is not evidence ([details](docs/REFERENCE.md#llm-assisted-explanation)).

`scripts/agent_fix.py` (experimental) plants a known lifetime bug in a scratch copy of the async-copy harness and asks a model for a patch; only the checker and an exhaustive run of every completion order can accept it. The five planted bugs, their transcripts, and token costs are in [docs/agent-fix](docs/agent-fix/README.md) ([details](docs/REFERENCE.md#agent-fix-loop-experimental)).

To instrument your own code, start with the
[instrumented example](examples/instrumented_demo.cpp) and the
[public headers](include/stateguard/stateguard.hpp). `stateguard::TraceRecorder`
captures events into a fixed-size buffer (`LockedTraceRecorder` for several
threads); a capture that overflows is never reported `clean`. An installed
package is used with `find_package(stateguard 0.2.0 EXACT CONFIG)` and
`stateguard::stateguard` ([install](docs/BUILDING.md#install-and-uninstall)). The [technical reference](docs/REFERENCE.md) covers every
rule, the reducer, and the generator; the [trace format](docs/TRACE-FORMAT.md)
is specified separately.

## Catch a reclaim bug in running code

The [async-copy harness](harness/async_copy.hpp) is a buffer pool, a copy engine
with controllable completions, and a cache host that shares one prefix between
two requests. The demo runs a safe host and one that reclaims buffers while a copy
still reads them:

```sh
./build/stateguard-async-copy-demo --trace-dir build
```

```text
safe deferred reclamation: clean
  capture: 37 events kept, 0 dropped, capacity 1024; access completion recorded by the engine
  harness: 0 unsafe reclaims; request-2 received prefix-1 intact
induced immediate reclamation: violation
  first finding: 5 premature_reclaim: allocation reclaimed while obligations remain (operation lease is still active)
    allocation=slot-0
    generation=1
```

In the buggy run request-2 really receives the wrong bytes. Further runs show
lost coverage reported as `inconclusive`, and a failed load: retrying it is
`clean`, publishing it anyway is `failed_write_publication`. Traces are written
to `build/<name>.sgtrace`.

## On a CUDA GPU

Configure with `-DSTATEGUARD_ENABLE_CUDA=ON` to build `stateguard-cuda-copy-demo`.
It copies 64 MiB between device buffers with `cudaMemcpyAsync`, records completion
only after a CUDA event confirms it, and in the buggy run refills the reclaimed
source on a second stream. Checked at tag v0.1.0 on a Tesla T4 (driver 580.82.07,
CUDA 13.0), 500 runs each:

| Configuration | Checker | Bytes received |
|---|---|---|
| Safe deferred reclamation | `clean` 500/500 | intact 500/500 |
| Safe deferred reclamation, copy start delayed 20 ms | `clean` 500/500 | intact 500/500 |
| Immediate reclamation | `premature_reclaim` 500/500 | intact 500/500 |
| Immediate reclamation, copy start delayed 20 ms | `premature_reclaim` 500/500 | corrupted 500/500 |

The same machine checked 6,000 generated histories with planted faults, every
verdict as planted ([details](docs/REFERENCE.md#cuda-copy-harness)).

Whether the early reclaim corrupts data depends on timing; the checker reports
the unsafe ordering either way. [`scripts/colab_gpu_check.sh`](scripts/colab_gpu_check.sh)
reruns this on a single-GPU machine such as Google Colab. This is one GPU; it says
nothing about RDMA/NIXL, multi-GPU transfers, or serving engines.

Stateguard diagnoses recorded events. It does not prevent a device from writing to
memory.

## Releases

Release binaries and a checksummed source archive are published on the [GitHub Releases](https://github.com/kernelworks-com/stateguard/releases) page.
In 0.x the CMake package matches versions exactly.

## Tests and feedback

```sh
ctest --test-dir build --output-on-failure --no-tests=error
```

[Report a problem](https://github.com/kernelworks-com/stateguard/issues) with a small
synthetic trace and the result you expected. Do not attach production captures.
CI setup is described in [BUILDING.md](docs/BUILDING.md#continuous-integration).

Stateguard is an independent [Kernelworks](https://github.com/kernelworks-com)
project. Licensed under [Apache-2.0](LICENSE).
