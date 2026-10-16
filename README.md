# Stateguard

**Find when cached inference state is reused before its users are finished.**

A memory address can still exist while the state stored there belongs to a
different request or allocation generation. Stateguard checks recorded lifecycle
events to help inference engineers spot those mistakes and identify the operation
or allocation involved.

**Available today:** an experimental C++ CPU checker, a trace CLI, a reducer that
shrinks a failing trace to a small witness, a deterministic generator of synthetic
lifecycle histories, a bounded event recorder for instrumented code, and an
instrumented CPU buffer-pool and async-copy harness. It runs locally without a GPU.
Connecting it to a serving engine requires instrumentation; there is no ready-made
engine adapter yet.

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
| `inconclusive` | Events or completion evidence are missing. Findings after lost events are listed as `[unconfirmed]`. | 2 |
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

## Shrink a failing trace

A captured trace can contain thousands of unrelated events. `stateguard-reduce`
keeps only the events needed to reproduce the same diagnostic:

```sh
./build/stateguard-reduce tests/fixtures/premature_reclaim.sgtrace
```

The reduced trace goes to standard output (or `--output FILE`), followed by a
summary on standard error:

```text
STATEGUARD_TRACE|1
allocate|buffer-1|1|4096|cpu
submit|copy-1|request-1|r:buffer-1:1|
retire|buffer-1|1
reclaim|buffer-1|1
end|complete
target: violation premature_reclaim at event 4: allocation reclaimed while obligations remain (operation lease is still active)
  allocation=buffer-1
  generation=1
reduced 6 -> 5 events in 40 checks (1-minimal)
kept original events: 0 2 3 4 5
```

The reducer only deletes events. It never reorders or rewrites them, and it
rejects any deletion that would create a failure the original trace did not
have. Choose a different failure with `--category NAME`. The command exits with
**0** after a reduction, **1** if the trace has no failure to reduce, and **3** for
an input error.

To test your own tooling, `stateguard-generate` writes a random but valid history
for a seed. It can also plant one known fault:

```sh
./build/stateguard-generate --seed 7 --fault premature_reclaim --output generated.sgtrace
./build/stateguard-check generated.sgtrace
./build/stateguard-reduce generated.sgtrace
```

The same seed and options produce the same trace on every platform. List the
available faults with `--list-faults`.

## Use it with your own events

Start with the [instrumented CPU example](examples/instrumented_demo.cpp), which
constructs events and runs the checker. Run it with:

```sh
./build/stateguard-instrumented-demo
```

For integration, emit typed events through the [public headers](include/stateguard/stateguard.hpp)
or follow the [trace fixtures](tests/fixtures). `stateguard::TraceRecorder` collects
events from running code into a fixed-size buffer and marks any events it had to
drop, so a capture that overflows is never reported `clean`. The
[technical reference](docs/REFERENCE.md) explains descriptors, publication proofs,
cancellation, and trace coverage.

## Catch a reclaim bug in running code

The [async-copy harness](harness/async_copy.hpp) is a small buffer pool, a copy
engine whose completions you control, and a cache host that shares one prefix
between two requests. It records its own events while it runs. The demo runs it
with a safe policy and with a deliberate bug: buffers are reclaimed as soon as the
host releases them, even while a copy still reads them.

```sh
./build/stateguard-async-copy-demo --trace-dir build
```

Its output starts with:

```text
safe deferred reclamation: clean
  capture: 37 events kept, 0 dropped, capacity 1024; access completion recorded by the engine
  harness: 0 unsafe reclaims; request-2 received prefix-1 intact
induced immediate reclamation: violation
  first finding: 5 premature_reclaim: allocation reclaimed while obligations remain (operation lease is still active)
    allocation=slot-0
    generation=1
```

In the buggy run, request-2 really receives the wrong bytes, and the checker
names the first buffer that was reclaimed too early. Two more runs show lost
coverage: a capture buffer that fills up, and a copy engine that cannot report when
a copy finished. Both are `inconclusive`. The demo writes each trace to
`build/<name>.sgtrace`. Run `./build/stateguard-reduce build/immediate.sgtrace` to
shrink the buggy run to a 7-event witness.

The harness runs on the CPU. CUDA, RDMA/NIXL, serving-engine adapters, and
timeline reporting are future work. Stateguard diagnoses recorded events; it does
not prevent a device from writing to memory.

## Tests and feedback

```sh
ctest --test-dir build --output-on-failure --no-tests=error
```

For build automation, see [CI details](.github/CI.md).
[Report a problem](https://github.com/kernelworks-com/stateguard/issues) with a small
synthetic trace and your expected result. Do not attach private production captures.

Stateguard is an independent [Kernelworks](https://github.com/kernelworks-com)
project; no sibling project is required. Licensed under [Apache-2.0](LICENSE).
