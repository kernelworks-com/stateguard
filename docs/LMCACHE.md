# LMCache static NIXL put capture (experimental)

This optional Python adapter exercises the CPU mocked static-put regression from
[LMCache PR #4658](https://github.com/LMCache/LMCache/pull/4658). It adds no Python,
torch, LMCache, or NIXL dependency to the C++ library or its build. Its hooks are
internal methods at these revisions, **not a stable LMCache API**:

| Revision | Role |
| --- | --- |
| `919fe8d67369cfe1dea85b28725f71a611369e1a` | Before the fix |
| `217b1d8248fdf63e1c6eede75a3ac5cde998a7b7` | Merged fix and regression helpers |

## Setup and reproduction

Use Python 3.11 and two separate source checkouts. For each pin, initialize an
empty directory with `git init`, add `https://github.com/LMCache/LMCache.git` as
`origin`, fetch the full SHA with `git fetch --depth 1 origin <sha>`, then use
`git checkout --detach FETCH_HEAD`. Keep the fixed checkout's test file intact;
the adapter runner verifies its bytes against the fixed commit. Never install
LMCache from PyPI for this experiment.

In a scratch virtual environment, install CPU torch from
`https://download.pytorch.org/whl/cpu`, then pytest, `numpy<=2.2.6`, msgspec,
pyyaml, psutil, sortedcontainers, cachetools, blake3, prometheus_client,
py-cpuinfo, aiohttp, requests, numba, aiofile, pyzmq, transformers, cryptography,
pybind11, `setuptools<81`, and setuptools_scm. Dependencies besides the engine
pins are not locked; the verified environment used Python 3.11 and torch 2.14.1
CPU on macOS arm64. Build each checkout's native extension in place:

```sh
NO_GPU_EXT=1 python setup.py build_ext --inplace
```

For macOS SDK include troubleshooting, see [BUILDING.md](BUILDING.md). Build the
Stateguard CLI using the [quick start](../README.md). From the Stateguard root,
using that Python environment, run this command once per engine checkout:

```sh
python -m adapters.lmcache.regression \
  --checkout /path/to/engine-checkout \
  --upstream-tests /path/to/fixed-checkout/tests/v1/test_nixl_put_refcount.py \
  --checker build/stateguard-check \
  --output /path/to/scratch-results --repeat 5 --benchmark-repeat 10
```

Run the independent upstream baseline from each engine root:

```sh
PYTHONPATH=. python -m pytest --noconftest tests/v1/test_nixl_put_refcount.py
```

For the vulnerable checkout only, first copy the fixed pin's test file into the
same relative location. `--noconftest` avoids importing the full serving engine.
These tests use mocked NIXL and real CPU TensorMemoryObj/reference-count methods.
They do not establish hardware support or production serving compatibility.

Dependency-free adapter checks run separately:

```sh
python3 -m adapters.lmcache.test_adapter --checker build/stateguard-check
```

## Events and completion evidence

`instrument(backend, writer)` returns a context manager restoring instance hooks
on exit. Install on an empty, idle static backend; drain the loop and stop all
producers before closing it or finishing the writer. Calls must use the wrapped
instance methods, including `batched_submit_put_task`. Calling a saved unbound
method bypasses the submit hook. Existing objects need observation through submit
before their references change. Do not change the pool, agent, or progress set
while installed.

The capture observes:

- Tensor page addresses mapped to opaque IDs, one allocation per observed page
  lifetime, and an increasing generation on reuse. Ref counts reaching zero emit
  retire/reclaim. Raw addresses and cache key values are not written to the trace.
  Pinning and overlapping live object aliases are rejected. The default address
  function uses `raw_data.data_ptr()`; custom page identity must be consistent.
- A source read obligation when `mem_to_storage` is scheduled, covering the window
  before the coroutine starts. `post_blocking` adds the actual transfer's source
  read and storage-slot write leases. Successful return completes both physical
  access obligations. Slot pop/push emits allocate and retire/reclaim with a new
  generation on subsequent reuse. Slot byte extent is unknown and recorded as 0.
- A separate zero-byte `put-metadata` allocation and `cleanup-*` operation represent
  the outstanding key/progress bookkeeping. Only all corresponding progress-set
  discards end this operation. It does **not** lease source bytes after transfer
  completion. On the vulnerable failure path it remains unresolved; the checker
  reports incomplete evidence, not a proven physical lifetime violation.
- Publication when a key remains in `key_dict` and its in-flight marker is
  discarded. Failed-put rollback removes the key first, so it does not publish.
  A non-None `_collect_metadata_with_lock` result emits consume. The engine's
  contains-before-get contract hides in-flight keys; direct in-flight reads are
  outside that contract and can be diagnosed without claiming a new upstream bug.

Successful `post_blocking` return at these pins observes NIXL `DONE`. An exception
alone is **not** assumed to prove quiescence: by default the adapter records a
coverage gap and leaves physical completion unknown. The CPU regression runner
sets `failed_post_is_quiescent=True` because its injected exception occurs in a
mock that performs no asynchronous access. That option emits failed completion;
it must not be enabled for a real backend without a separately established
completion contract. Handle release, coroutine return, timeout, and progress
removal alone are not physical completion proofs.

Semantic descriptors are deliberately opaque and limited to a capture's key/state
mapping. This adapter checks lifetime and visibility; it does not verify KV tensor
contents, model configuration, hash collisions, or semantic equality across keys.

## Ordering, bounds, and limits

The writer uses a reentrant lock. Captured transactions follow lock acquisition
order, used as the observation order within one process; this does not reconstruct
device or distributed happens-before. Blocking transport calls run outside the
capture lock so source ref changes remain observable. Hooks can perturb scheduling.
The tested concurrent engine pattern is one event-loop put plus caller ref drops;
arbitrary concurrent submit/eviction/read workloads have not been validated.
In particular, nested backend/allocator locks require a separate lock-order audit
before using this experimental capture under those workloads.

`TraceWriter(capacity=100000, max_record_bytes=4096)` bounds retained event strings
by count and per-record UTF-8 length. Overflow drops the tail and reserves exactly
one `coverage_gap|capture-overflow` plus `end|incomplete`; it cannot become clean.
Schema-1 integer bounds, nonzero generations, delimiters, and descriptor shape are
validated before serialization. IDs contain none of `|;:,` or newlines. The writer
supports the schema-1 subset emitted here, not routed transfer events.

Identity maps and hook references live for the capture session; those are **not**
bounded by the event capacity. Keep sessions finite. The adapter retains Python
object references but never increments LMCache ref counts. Restore hooks to release
those references. Reentrancy into instrumented allocation from instrumentation,
resurrection of a reclaimed object, pin/unpin, dynamic storage, actual get transfers,
partial acquisition failures, and cancellation are outside the validated scope.
Unsupported operations may raise; this is an experimental diagnostic harness,
not a transparent production plugin. Multi-thread writer checks exercise 1,000
transactions on eight workers; they do not establish arbitrary engine concurrency.

## Verified results

See [REFERENCE.md](REFERENCE.md#lmcache-static-put-adapter) for counts and timings.
Each runner invocation writes synthetic traces and a JSON experiment record to its
chosen scratch output. Keep raw records separate from public source delivery.
