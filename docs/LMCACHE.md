# LMCache NIXL put capture (experimental)

This optional Python adapter exercises the CPU mocked static- and dynamic-put
regressions from
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
CPU (static put, 2026-10-20) and torch 2.15.0 CPU (static and dynamic put,
2026-11-27) on macOS arm64. Build each checkout's native extension in place:

```sh
NO_GPU_EXT=1 python setup.py build_ext --inplace
```

For macOS SDK include troubleshooting, see [BUILDING.md](BUILDING.md); with an
SDK whose C++ headers are not found, also pass
`CXXFLAGS="-isystem$(xcrun --show-sdk-path)/usr/include/c++/v1"`. Build the
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

The adapter writes traces with the [Python trace writer](REFERENCE.md#python-trace-writer)
(`stateguard_trace`, in `python/`); `adapters/lmcache/trace.py` re-exports it. Install
it with `pip install -e python/` into the same environment, or run from the
Stateguard root, where the checkout's copy is used when the package is not installed.

## Events and completion evidence

`instrument(backend, writer=None, *, put_path='static', clock=False)` returns a
context manager restoring instance hooks on exit; its `writer` attribute is the
writer in use (a new `TraceWriter` when none is passed). `put_path='dynamic'`
instruments a `NixlDynamicStorageBackend` instead (see
[Dynamic put path](#dynamic-put-path)). Install on an empty, idle backend; drain
the loop and stop all producers before closing it or ending the writer
(`writer.end()`). Calls must use the wrapped
instance methods, including `batched_submit_put_task`. Calling a saved unbound
method bypasses the submit hook. Existing objects need observation through submit
before their references change. Do not change the pool, agent, or progress set
while installed.

The static capture observes:

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
- Binding of a slot to a new state when `add_key_to_dict` is entered, recorded
  before the key is inserted (so no eviction can be recorded ahead of it); a
  failed insert removes the key mapping again.
- Publication when a key remains in `key_dict` and its in-flight marker is
  discarded. Failed-put rollback removes the key first, so it does not publish.
  A non-None `_collect_metadata_with_lock` result emits consume. The engine's
  contains-before-get contract hides in-flight keys; direct in-flight reads are
  outside that contract and can be diagnosed without claiming a new upstream bug.

Successful `post_blocking` return at these pins observes NIXL `DONE`. An exception
alone is **not** assumed to prove quiescence: by default the adapter records a
coverage gap and leaves physical completion unknown (see
[Quiescence contract](#quiescence-contract)). Handle release, coroutine return,
timeout, and progress removal alone are not physical completion proofs.

Semantic descriptors are deliberately opaque and limited to a capture's key/state
mapping. This adapter checks lifetime and visibility; it does not verify KV tensor
contents, model configuration, hash collisions, or semantic equality across keys.

## Dynamic put path

Both pins have the same dynamic put surface (`NixlDynamicStorageBackend`), so
`put_path='dynamic'` is checked at both. The page, hold, and cleanup events are
those of the static path; the storage side differs:

- `_acquire_storage_handle(..., write=True)` returning allocates one storage
  object per key (`object-*`, the page size as its extent, a new generation when a
  removed key is stored again) and binds it to a new state. Re-putting a key whose
  object is still live raises. If acquisition raises, no handle exists, so the
  hold operation completes as failed and nothing else is recorded.
- `agent.post_async` (async mode) or `agent.post_blocking` (sync mode) submits the
  transfer with source read and object write leases. In async mode, completion
  evidence is the `agent.nixl_agent.check_xfer_state` (or `post_async`) result for
  that handle: `DONE` completes the transfer; `ERR` and a raised post are failures
  under the quiescence rule below.
- Publication when the key leaves the progress set after a `DONE` was observed for
  its transfer; `contains()` hides the key until then. A failed write never
  publishes. Its object is retired and reclaimed at that discard only if the
  transfer's end is known (quiescent failure, or no post); otherwise it stays live.
- `remove(key)` retires and reclaims a published object.

Not covered: dynamic get transfers (`storage_to_mem`) and their consume events,
the presence cache and `key_exists` queries as visibility sources, FILE descriptor
and unlink side effects, partially written OBJ objects that a storage query might
still report, and concurrent puts of the same key. The upstream mock replaces
`_acquire_storage_handle`, so registration and descriptor code is not exercised.

## Locking

Locks involved at both pins: the backend's `key_lock` and `progress_lock`
(reentrant in the engine, plain locks in the upstream mocks), the descriptor
pool's lock (`NixlDescPool.lock`), each `TensorMemoryObj.lock` (taken by ref-count
changes, which take the allocator's lock when the count reaches zero), and the
writer's reentrant lock. The adapter relies on this order and never acquires an
earlier lock while holding a later one:

1. `key_lock`
2. `progress_lock`
3. writer lock
4. pool lock, or a `MemoryObj` lock and then the allocator lock

Where each hook records:

| Hook | Locks held by the engine | Adapter |
| --- | --- | --- |
| `batched_submit_put_task` | none | writer lock while tracking sources, released before the original takes `key_lock` and `progress_lock` |
| `pool.push` during submit-time eviction | `key_lock`, `progress_lock` | writer lock, then the pool lock |
| `pool.pop`, `pool.push` elsewhere | none | writer lock, then the pool lock |
| `add_key_to_dict` | none (the original takes `key_lock`) | writer lock to record the binding, released before the original runs |
| `progress_set.discard` | `progress_lock` | writer lock; reads `key_dict` without `key_lock` |
| `ref_count_up`, `ref_count_down` | none at these pins | writer lock, then the `MemoryObj` lock |
| `_collect_metadata_with_lock` | none (the original takes `key_lock`) | writer lock after the original returns |
| `post_blocking`, `post_async`, `check_xfer_state`, `_acquire_storage_handle` | none | writer lock only before and after the call, never across it |
| dynamic `remove` | none | writer lock across the original, which takes no backend lock |

Bug found and fixed: the previous `add_key_to_dict` hook held the writer lock
while the original took `key_lock`, while submit-time eviction holds `key_lock`
and needs the writer lock to record `pool.push`. A put in flight on the event loop
and a submit that evicts could deadlock. The hook now records the binding first
and runs the original without the writer lock. Recording the binding after the
original instead would let an eviction's retire/reclaim appear before the binding.
Both interleavings are forced with events in the dependency-free tests.

Ordering the adapter does not make exact:

- Completion is recorded after the transport call returns, so a ref drop that
  races with the end of a transfer can be recorded first. When the engine has no
  ordering edge between the two (the vulnerable static pin), the reported
  `premature_reclaim` is that missing edge, even if the run itself did not
  corrupt memory.
- `consume` is recorded after `key_lock` is released, so an eviction in between is
  recorded first. That is conservative for the read that follows.
- The publication check reads `key_dict` without `key_lock`; taking it there would
  invert `key_lock` and `progress_lock`. An eviction racing with the discard can
  yield a publication of a key that was just removed, followed by its reclaim.
- Locks the adapter does not see, such as custom cache policies, presence caches,
  allocator callbacks, or NIXL's own threads, are outside the audit. Custom pools
  or allocators that call back into instrumented methods are unsupported.

## Quiescence contract

`failed_post_is_quiescent=True` records a failed transfer (`post_blocking` or
`post_async` raising, or `check_xfer_state` returning `ERR`) as a failed
completion, which ends its leases. Without it, the adapter records
`coverage_gap|unproven-failed-transfer-completion` and the checker's result is
inconclusive. The adapter refuses the option unless `quiescent_transport` names
the transport whose failure behaviour was established; the CPU runner and tests
pass `quiescent_transport='mock'`, because their injected failures perform no
asynchronous access. The name is a deliberate acknowledgement, not a check: the
adapter cannot verify it.

Before the option may be enabled for a real transport, that transport must
guarantee, for the failed handle: no NIXL backend, progress thread, NIC, or GPU
engine reads the source pages or writes the storage descriptors after the failure
is reported to LMCache, including retries, partially issued work requests, and
operations still queued in hardware. In other words, the failure must be reported
only once the handle is in a terminal state in which every access it issued has
ended.

The signal that would prove it is a terminal transfer state for that handle that
the transport documents as quiescent, for example `ERR` from `check_xfer_state`
followed by a successful `release_xfer_handle` if NIXL documents that release
fails while an issued transfer can still access memory (the fixed pin's comments
assume this for active transfers). Whether NIXL and each backend plugin (POSIX,
GDS, OBJ, UCX) give that guarantee is not established here; that remains an open
requirement and needs a real transport to test.

Enabled wrongly, the trace ends the leases early. Rollback then returns the slot,
storage object, or source page, and the checker reports a clean rollback while a
late write can still land in memory that has been reused. The checker cannot
detect this from the trace; that is why the option is refused by default.

## Ordering, bounds, and limits

The writer uses a reentrant lock. Captured transactions follow lock acquisition
order, used as the observation order within one process; this does not reconstruct
device or distributed happens-before. Blocking transport calls run outside the
capture lock so source ref changes remain observable. Hooks can perturb scheduling.
The tested concurrent patterns are one event-loop put plus caller ref drops, and
the forced lock-order interleavings in [Locking](#locking); arbitrary concurrent
submit/eviction/read workloads have not been run.

`TraceWriter(capacity=100000, max_record_bytes=4096)` bounds retained event strings
by count and per-record UTF-8 length. Overflow drops the tail and reserves exactly
one `coverage_gap|capture-overflow` plus `end|incomplete`; it cannot become clean.
Schema-1 integer bounds, nonzero generations, delimiters, and descriptor shape are
validated before serialization. IDs contain none of `|;:,` or newlines. The writer
covers all of schema 1; this adapter emits no routed transfer events. Pass
`clock=True` to `instrument` (or a `TraceWriter(clock=True)`) to stamp the
capture with `time|monotonic|<ns>` lines for `stateguard-timeline`; the reading is
taken when the hook records the event. Puts have no cancellation at these pins, so
the adapter records no `cancel` events; a timestamped capture gives a complete
timeline with no requests.

Identity maps and hook references live for the capture session; those are **not**
bounded by the event capacity. Keep sessions finite. The adapter retains Python
object references but never increments LMCache ref counts. Restore hooks to release
those references. Reentrancy into instrumented allocation from instrumentation,
resurrection of a reclaimed object, pin/unpin, actual get transfers, partial
acquisition failures of the static path, and cancellation are outside the
validated scope. Unsupported operations may raise; this is an experimental
diagnostic harness, not a transparent production plugin. Multi-thread writer
checks exercise 1,000 transactions on eight workers; they do not establish
arbitrary engine concurrency.

## Verified results

See [REFERENCE.md](REFERENCE.md#lmcache-put-adapter) for counts and timings.
Each runner invocation writes synthetic traces and a JSON experiment record to its
chosen scratch output. Keep raw records separate from public source delivery.
