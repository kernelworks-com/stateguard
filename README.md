# Stateguard

**Find when cached inference state is reused before its users are finished.**

A memory address can still exist while the state stored there belongs to a
different request or allocation generation. Stateguard checks recorded lifecycle
events to help inference engineers spot those mistakes and identify the operation
or allocation involved.

**Available today:** an experimental C++ CPU checker, a trace CLI, and synthetic
examples. It runs locally without a GPU. Connecting it to a serving engine requires
instrumentation; there is no ready-made engine adapter yet.

## Try a safe trace and a broken one

You need a C++20 compiler and CMake 3.20+.
[Build help](docs/BUILDING.md) covers installation and common macOS errors.

```sh
git clone https://github.com/kernelworks-com/stateguard.git
cd stateguard
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
./build/stateguard-check tests/fixtures/safe_deferred_reclaim.sgtrace
```

Already in the repository? Start at the `cmake` command. The safe trace prints:

```text
clean
```

Now check the deliberately broken trace:

```sh
./build/stateguard-check tests/fixtures/premature_reclaim.sgtrace
```

Its output starts with:

```text
violation
4 premature_reclaim: allocation reclaimed while obligations remain (operation lease is still active)
  allocation=buffer-1
  generation=1
```

The buffer was reclaimed while an operation could still access it. This command
intentionally exits with code **1** because it found a violation.

## Understand the result

| Result | Meaning | Exit code |
|---|---|---|
| `clean` | No modeled violation found in a trace with complete required evidence. | 0 |
| `violation` | An observed event breaks a checked lifetime or identity rule. | 1 |
| `inconclusive` | Events or completion evidence are missing. | 2 |
| Input error | The file, command, or trace format could not be accepted. | 3 |

A clean result applies to the supplied events and modeled rules. It does not prove
that an uninstrumented engine or every inference result is correct.

## What you can check

- Reuse or consumption with a stale allocation generation or mismatched state.
- Reclaiming storage while a consumer or operation still holds it.
- Writing to published immutable state, or publishing before writers finish.
- Invalid lifecycle transitions, orphaned operations, and duplicate completions.
- Missing completion evidence and gaps in capture coverage.

Legitimate shared state is allowed. Cancelling a request does not automatically
release an operation's access to memory.

## Use it with your own events

Start with the [instrumented CPU example](examples/instrumented_demo.cpp), which
constructs events and runs the checker. Run it with:

```sh
./build/stateguard-instrumented-demo
```

For integration, emit typed events through the [public headers](include/stateguard/stateguard.hpp)
or follow the [trace fixtures](tests/fixtures). The [technical reference](docs/REFERENCE.md)
explains descriptors, publication proofs, cancellation, and trace coverage.

CUDA, RDMA/NIXL, serving-engine adapters, trace reduction, and timeline reporting
are future work. Stateguard diagnoses recorded events; it does not prevent a device
from writing to memory.

## Tests and feedback

```sh
ctest --test-dir build --output-on-failure --no-tests=error
```

For build automation, see [CI details](.github/CI.md).
[Report a problem](https://github.com/kernelworks-com/stateguard/issues) with a small
synthetic trace and your expected result. Do not attach private production captures.

Stateguard is an independent [Kernelworks](https://github.com/kernelworks-com)
project; no sibling project is required. Licensed under [Apache-2.0](LICENSE).
