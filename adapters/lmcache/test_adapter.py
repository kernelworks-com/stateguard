"""Dependency-free capture checks; CLI conformance checks use --checker."""
import argparse
import asyncio
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

from .trace import TraceWriter, identifier, uint
from .adapter import Capture, _Progress, instrument
from types import SimpleNamespace

import stateguard_trace

CHECKER = None
TIMEOUT = 5


class WriterTests(unittest.TestCase):
    def report(self, trace):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.sgtrace'
            path.write_text(trace)
            return subprocess.run([str(CHECKER), str(path)], capture_output=True, text=True)

    def test_identifiers(self):
        for text in ('', 'x|y', 'x;y', 'x:y', 'x,y', 'x\ny', 'x\ry'):
            with self.assertRaises(ValueError):
                identifier(text)
        self.assertEqual(identifier('page-1'), 'page-1')

    def test_integer_bounds(self):
        for value in (0, -1, 1 << 64, True, 1.5):
            with self.assertRaises(ValueError):
                uint(value, True)
        self.assertEqual(uint((1 << 64) - 1, True), '18446744073709551615')

    def test_closed(self):
        writer = TraceWriter()
        writer.end()
        with self.assertRaises(RuntimeError):
            writer.allocate('page', 1, 4, 'cpu')

    def test_overflow(self):
        writer = TraceWriter(1)
        writer.allocate('page', 1, 4, 'cpu')
        for _ in range(100):
            writer.reclaim('page', 1)
        trace = writer.end()
        self.assertEqual(len(trace.splitlines()), 4)
        self.assertEqual(trace.count('coverage_gap'), 1)
        self.assertTrue(trace.endswith('end|incomplete\n'))
        if CHECKER:
            self.assertEqual(self.report(trace).returncode, 2)

    def test_zero_capacity_and_record_bound(self):
        for writer in (TraceWriter(0), TraceWriter(10, 64)):
            writer.allocate('a' * 65, 1, 0, 'cpu')
            self.assertIn('coverage_gap|capture-overflow', writer.end())

    def test_concurrent_transactions(self):
        writer = TraceWriter(4000)
        def transaction(n):
            with writer.lock:
                name = f'page-{n}'
                writer.allocate(name, 1, 32, 'cpu')
                writer.retire(name, 1)
                writer.reclaim(name, 1)
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(transaction, range(1000)))
        trace = writer.end()
        self.assertEqual(len(trace.splitlines()), 3002)
        if CHECKER:
            result = self.report(trace)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_wire_format(self):
        writer = TraceWriter()
        writer.allocate('page', 1, 32, 'cpu')
        writer.submit('copy', 'put', [('r', ('page', 1))])
        writer.complete('copy', 'failed')
        writer.retire('page', 1)
        writer.reclaim('page', 1)
        self.assertEqual(writer.end(), 'STATEGUARD_TRACE|1\nallocate|page|1|32|cpu\n'
                         'submit|copy|put|r:page:1|\ncomplete|copy|failed\n'
                         'retire|page|1\nreclaim|page|1\nend|complete\n')


class Object:
    def __init__(self):
        self.meta = SimpleNamespace(pin_count=0)
        self.count = 1

    def get_ref_count(self):
        return self.count

    def get_physical_size(self):
        return 32

    def ref_count_up(self):
        self.count += 1

    def ref_count_down(self):
        self.count -= 1

    def pin(self):
        self.meta.pin_count += 1

    def unpin(self):
        self.meta.pin_count -= 1


class CaptureTests(unittest.TestCase):
    report = WriterTests.report
    def test_page_reuse_and_restoration(self):
        writer = TraceWriter()
        capture = Capture(None, writer, lambda obj: 42)
        first, second = Object(), Object()
        first_ref = capture.track(first)
        first.ref_count_up()
        first.ref_count_down()
        first.ref_count_down()
        second_ref = capture.track(second)
        self.assertEqual(second_ref, (first_ref[0], 2))
        second.ref_count_down()
        capture.close()
        self.assertNotIn('ref_count_down', first.__dict__)
        self.assertNotIn('pin', first.__dict__)
        trace = writer.end()
        if CHECKER:
            result = self.report(trace)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_live_alias_rejected(self):
        capture = Capture(None, TraceWriter(), lambda obj: 42)
        try:
            capture.track(Object())
            with self.assertRaises(ValueError):
                capture.track(Object())
        finally:
            capture.close()

    def test_pin_rejected(self):
        capture = Capture(None, TraceWriter(), lambda obj: 42)
        obj = Object()
        try:
            capture.track(obj)
            with self.assertRaises(ValueError):
                obj.pin()
            self.assertEqual(obj.meta.pin_count, 0)
        finally:
            capture.close()

    def test_slot_reuse(self):
        pool = SimpleNamespace(pop=lambda: 7, push=lambda index: None)
        backend = SimpleNamespace(progress_set=set(), key_dict={}, pool=pool,
                                  agent=SimpleNamespace(post_blocking=lambda handle: None))
        for name in ('batched_submit_put_task', 'mem_to_storage', 'add_key_to_dict',
                     '_collect_metadata_with_lock'):
            setattr(backend, name, lambda *args, **kwargs: None)
        writer = TraceWriter()
        with instrument(backend, writer):
            for _ in range(2):
                self.assertEqual(backend.pool.pop(), 7)
                backend.pool.push(7)
        trace = writer.end()
        self.assertIn('allocate|slot-1|2|0|storage', trace)
        if CHECKER:
            result = self.report(trace)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_timestamped_capture(self):
        pool = SimpleNamespace(pop=lambda: 3, push=lambda index: None)
        backend = SimpleNamespace(progress_set=set(), key_dict={}, pool=pool,
                                  agent=SimpleNamespace(post_blocking=lambda handle: None))
        for name in ('batched_submit_put_task', 'mem_to_storage', 'add_key_to_dict',
                     '_collect_metadata_with_lock'):
            setattr(backend, name, lambda *args, **kwargs: None)
        writer = TraceWriter(clock=True)
        with instrument(backend, writer):
            backend.pool.pop()
            backend.pool.push(3)
        trace = writer.end()
        self.assertRegex(trace, r'^STATEGUARD_TRACE\|1\ntime\|monotonic\|[0-9]+\nallocate\|slot-1\|1\|')
        if CHECKER:
            result = self.report(trace)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_restore_progress_contents(self):
        original = set()
        backend = SimpleNamespace(progress_set=original)
        capture = Capture(backend, TraceWriter(), lambda obj: 42)
        capture.patch(backend, 'progress_set', _Progress(original, capture))
        backend.progress_set.add('pending')
        capture.close()
        self.assertIs(backend.progress_set, original)
        self.assertEqual(original, {'pending'})

    def test_metadata_format_validation(self):
        writer = TraceWriter()
        for call in (lambda: writer.allocate('page', 0, 4, 'cpu'),
                     lambda: writer.submit('op', 'req', [('r', 'bad:ref', 1)]),
                     lambda: writer.publish('state', 'bad,ref', 1),
                     lambda: writer.complete('op', 'unknown')):
            with self.assertRaises(ValueError):
                call()


def lock_is_free(lock):
    """True when another thread can take lock (it is not held by the caller)."""
    taken = []
    def probe():
        ok = lock.acquire(timeout=TIMEOUT)
        if ok:
            lock.release()
        taken.append(ok)
    thread = threading.Thread(target=probe)
    thread.start()
    thread.join()
    return taken[0]


def start(target):
    thread = threading.Thread(target=target, daemon=True)
    thread.start()
    return thread


def static_backend(add_key):
    """Static backend stub with the pinned revisions' lock types and nesting."""
    backend = SimpleNamespace(progress_set=set(), key_dict={}, key_lock=threading.RLock(),
                              progress_lock=threading.RLock())
    indices = [1, 0]
    pool_lock = threading.Lock()
    def pop():
        with pool_lock:
            return indices.pop()
    def push(index):
        with pool_lock:
            indices.append(index)
    backend.pool = SimpleNamespace(pop=pop, push=push)
    backend.agent = SimpleNamespace(post_blocking=lambda handle: None)
    for name in ('batched_submit_put_task', 'mem_to_storage', '_collect_metadata_with_lock'):
        setattr(backend, name, lambda *args, **kwargs: None)
    backend.add_key_to_dict = lambda key, meta, index: add_key(backend, key, index)
    return backend


class LockOrderTests(unittest.TestCase):
    """Interleavings forced with events; each fails under the previous hook order."""

    def test_submit_eviction_does_not_deadlock_with_add(self):
        # Eviction in batched_submit_put_task holds key_lock and progress_lock
        # while remove() pushes the slot; the loop thread is in add_key_to_dict.
        entered = threading.Event()
        def add_key(backend, key, index):
            entered.set()
            with backend.key_lock:
                backend.key_dict[key] = index
        backend = static_backend(add_key)
        writer = TraceWriter()
        holding = threading.Event()
        with instrument(backend, writer) as capture:
            evicted_slot, new_slot = backend.pool.pop(), backend.pool.pop()
            def evict():
                with backend.key_lock, backend.progress_lock:
                    holding.set()
                    self.assertTrue(entered.wait(TIMEOUT))
                    backend.pool.push(evicted_slot)
            def add():
                capture.current.set({'slots': []})
                backend.add_key_to_dict('key', None, new_slot)
            evictor = start(evict)
            self.assertTrue(holding.wait(TIMEOUT))
            adder = start(add)
            evictor.join(TIMEOUT)
            adder.join(TIMEOUT)
            self.assertFalse(evictor.is_alive() or adder.is_alive(),
                             'key_lock -> writer lock and writer lock -> key_lock deadlocked')
        self.assertEqual(backend.key_dict, {'key': new_slot})

    def test_bind_recorded_before_key_can_be_evicted(self):
        inserted, evicted = threading.Event(), threading.Event()
        waited = []
        def add_key(backend, key, index):
            with backend.key_lock:
                backend.key_dict[key] = index
            inserted.set()
            waited.append(evicted.wait(TIMEOUT))  # hook still running: it must not hold the writer lock
        backend = static_backend(add_key)
        writer = TraceWriter()
        with instrument(backend, writer) as capture:
            slot = backend.pool.pop()
            def add():
                capture.current.set({'slots': []})
                backend.add_key_to_dict('key', None, slot)
            adder = start(add)
            self.assertTrue(inserted.wait(TIMEOUT))
            with backend.key_lock:  # remove(): pop under key_lock, push after it
                index = backend.key_dict.pop('key')
            backend.pool.push(index)
            evicted.set()
            adder.join(TIMEOUT)
            self.assertFalse(adder.is_alive())
        self.assertEqual(waited, [True])
        lines = writer.end().splitlines()
        bind = next(i for i, line in enumerate(lines) if line.startswith('bind_state|slot-1|1|'))
        self.assertLess(bind, lines.index('retire|slot-1|1'))

    def test_failed_add_restores_key_map(self):
        def add_key(backend, key, index):
            raise AssertionError('key already present')
        backend = static_backend(add_key)
        with instrument(backend, TraceWriter()) as capture:
            slot = backend.pool.pop()
            context = {'slots': []}
            capture.current.set(context)
            with self.assertRaises(AssertionError):
                backend.add_key_to_dict('key', None, slot)
            self.assertEqual(context['slots'], [])
            self.assertNotIn('key', capture.keys)

    def test_dynamic_transport_and_backend_calls_run_without_writer_lock(self):
        with DynamicHarness(self, async_mode=True, probe=True) as harness:
            harness.put()
            harness.release()
        self.assertTrue(harness.backend.probes)
        self.assertTrue(all(harness.backend.probes.values()), harness.backend.probes)

    def test_static_transport_runs_without_writer_lock(self):
        backend = static_backend(lambda backend, key, index: None)
        writer = TraceWriter()
        probes = []
        backend.agent.post_blocking = lambda handle: probes.append(lock_is_free(writer.lock))
        with instrument(backend, writer) as capture:
            capture.current.set({'hold': 'put-0', 'sources': [], 'slots': [], 'failed': False})
            writer.submit('put-0', 'put')
            backend.agent.post_blocking(object())
        self.assertEqual(probes, [True])


# Dynamic put path ----------------------------------------------------------


class LoopThread:
    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.errors = []
        self.loop.set_exception_handler(lambda loop, context: self.errors.append(context))
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(TIMEOUT)
        self.loop.close()


class NixlAgent:
    def __init__(self, owner):
        self.owner = owner

    def check_xfer_state(self, handle):
        self.owner.probe('check_xfer_state')
        return self.owner.outcome if self.owner.gate.is_set() else 'PROC'


class DynamicAgent:
    mem_type = 'OBJ'

    def __init__(self, owner):
        self.owner = owner
        self.nixl_agent = NixlAgent(owner)

    def post_async(self, handle):
        self.owner.probe('post_async')
        self.owner.posted.set()
        if self.owner.post_error:
            raise RuntimeError('injected post failure')
        return 'PROC'

    def post_blocking(self, handle):
        self.owner.probe('post_blocking')
        self.owner.posted.set()
        if not self.owner.gate.wait(TIMEOUT):
            raise TimeoutError('transfer gate timed out')
        if self.owner.outcome == 'ERR':
            raise RuntimeError('NIXL transfer failed')

    def release_handle(self, handle):
        pass

    def release_storage_handler(self, reg_descs, xfer_handler, descs):
        pass


class DynamicBackend:
    """Standard-library stand-in for NixlDynamicStorageBackend's put path.

    Method names, call structure, and lock use follow the fixed pin.
    hold_source_ref=False plants a missing source reference in async mode;
    discard_on_failure=False keeps the earlier pin's failed-transfer behaviour
    (the key stays in progress forever).
    """

    def __init__(self, loop, async_mode, outcome='DONE', post_error=False, acquire_error=False,
                 hold_source_ref=True, discard_on_failure=True):
        self.loop = loop
        self.async_mode = async_mode
        self.outcome = outcome
        self.post_error = post_error
        self.acquire_error = acquire_error
        self.hold_source_ref = hold_source_ref
        self.discard_on_failure = discard_on_failure
        self.progress_lock = threading.Lock()
        self.progress_set = set()
        self.presence = set()
        self.gate, self.posted, self.settled = threading.Event(), threading.Event(), threading.Event()
        self.agent = DynamicAgent(self)
        self.writer_lock = None
        self.probes = {}

    def probe(self, name):
        if self.writer_lock is not None and name not in self.probes:
            self.probes[name] = lock_is_free(self.writer_lock)

    def exists_in_put_tasks(self, key):
        with self.progress_lock:
            return key in self.progress_set

    def contains(self, key):
        return not self.exists_in_put_tasks(key) and key in self.presence

    def _cache_add(self, key):
        self.presence.add(key)

    def remove(self, key, force=True):
        self.presence.discard(key)
        return True

    def _acquire_storage_handle(self, keys, mem_indices, storage_indices, page_size, write):
        self.probe('_acquire_storage_handle')
        if self.acquire_error:
            raise RuntimeError('injected registration failure')
        return [], object(), object(), object()

    def _discard(self, keys):
        with self.progress_lock:
            for key in keys:
                self.progress_set.discard(key)

    def _release_sources(self, mem_objs):
        if self.async_mode and self.hold_source_ref:
            for obj in mem_objs:
                obj.ref_count_down()

    def batched_submit_put_task(self, keys, memory_objs):
        with self.progress_lock:
            for key in keys:
                self.progress_set.add(key)
        if self.async_mode and self.hold_source_ref:
            for obj in memory_objs:
                obj.ref_count_up()
        future = asyncio.run_coroutine_threadsafe(self.mem_to_storage(keys, memory_objs), self.loop)
        if self.async_mode:
            return
        try:
            future.result(TIMEOUT)
        except Exception:
            self._discard(keys)

    async def mem_to_storage(self, keys, mem_objs):
        if not keys:
            return
        try:
            descs, reg_descs, xfer_handler, handle = self._acquire_storage_handle(
                keys, [0] * len(keys), range(len(keys)), 4096, write=True)
        except Exception:
            self._discard(keys)
            self._release_sources(mem_objs)
            self.settled.set()
            if self.async_mode:
                return
            raise
        if self.async_mode:
            self._submit_async_mem_to_storage(handle, keys, mem_objs)
        else:
            self._run_sync_mem_to_storage(handle, keys)

    def _submit_async_mem_to_storage(self, handle, keys, mem_objs):
        try:
            initial_state = self.agent.post_async(handle)
            asyncio.ensure_future(self._wait_for_transfer(handle, initial_state, keys, mem_objs))
        except Exception:
            self.agent.release_handle(handle)
            self._discard(keys)
            self._release_sources(mem_objs)
            self.settled.set()

    async def _wait_for_transfer(self, handle, initial_state, keys, mem_objs):
        state = initial_state
        try:
            while state not in ('DONE', 'ERR'):
                state = self.agent.nixl_agent.check_xfer_state(handle)
                await asyncio.sleep(0.001)
        finally:
            self.agent.release_handle(handle)
            if state == 'DONE':
                for key in keys:
                    self._cache_add(key)
            if state == 'DONE' or self.discard_on_failure:
                self._discard(keys)
            self._release_sources(mem_objs)
            self.settled.set()

    def _run_sync_mem_to_storage(self, handle, keys):
        try:
            self.agent.post_blocking(handle)
            for key in keys:
                self._cache_add(key)
        finally:
            self.agent.release_handle(handle)
            self._discard(keys)
            self.settled.set()


class DynamicHarness:
    """One put of one source object through an instrumented DynamicBackend."""

    def __init__(self, test, probe=False, writer=None, clock=False, quiescent=False, **options):
        self.test = test
        self.loop = LoopThread()
        self.backend = DynamicBackend(self.loop.loop, **options)
        self.source = Object()
        flags = {'failed_post_is_quiescent': True, 'quiescent_transport': 'mock'} if quiescent else {}
        self.capture = instrument(self.backend, writer, page_address=id, put_path='dynamic',
                                  clock=clock, **flags)
        self.writer = self.capture.writer
        if probe:
            self.backend.writer_lock = self.writer.lock

    def put(self):
        self.backend.batched_submit_put_task(['key'], [self.source]) if self.backend.async_mode else \
            threading.Thread(target=self.backend.batched_submit_put_task,
                             args=(['key'], [self.source]), daemon=True).start()
        return self

    def release(self):
        self.backend.gate.set()
        self.test.assertTrue(self.backend.settled.wait(TIMEOUT), 'put did not settle')
        # Barrier: the loop has finished the callbacks scheduled so far.
        asyncio.run_coroutine_threadsafe(asyncio.sleep(0), self.loop.loop).result(TIMEOUT)

    def finish(self):
        """Caller drops its reference after the put; returns (trace, visible)."""
        visible = self.backend.contains('key')
        if self.source.get_ref_count() > 0:
            self.source.ref_count_down()
        return self.writer.end(), visible

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.backend.gate.set()
        self.loop.close()
        self.capture.close()


class DynamicPutTests(unittest.TestCase):
    report = WriterTests.report

    def verdict(self, trace, code, *needles):
        if CHECKER:
            result = self.report(trace)
            self.assertEqual(result.returncode, code, result.stdout)
            for needle in needles:
                self.assertIn(needle, result.stdout)

    def test_async_put_publishes_after_completion(self):
        with DynamicHarness(self, async_mode=True) as harness:
            harness.put()
            self.assertTrue(harness.backend.posted.wait(TIMEOUT))
            self.assertFalse(harness.backend.contains('key'))
            harness.release()
            trace, visible = harness.finish()
        self.assertTrue(visible)
        lines = trace.splitlines()
        for event in ('allocate|object-', 'bind_state|object-', 'submit|transfer-', 'complete|transfer-',
                      'publish|state-', 'retire|page-', 'reclaim|page-'):
            self.assertTrue(any(line.startswith(event) for line in lines), event)
        complete = next(i for i, line in enumerate(lines) if line.startswith('complete|transfer-'))
        publish = next(i for i, line in enumerate(lines) if line.startswith('publish|'))
        self.assertLess(complete, publish)
        self.assertTrue(lines[publish].endswith('|transfer-' + lines[complete].split('-')[1]))
        self.verdict(trace, 0)

    def test_async_caller_drop_during_transfer_is_clean(self):
        with DynamicHarness(self, async_mode=True) as harness:
            harness.put()
            self.assertTrue(harness.backend.posted.wait(TIMEOUT))
            harness.source.ref_count_down()  # backend still holds its reference
            self.assertEqual(harness.source.get_ref_count(), 1)
            harness.release()
            self.assertEqual(harness.source.get_ref_count(), 0)
            trace, visible = harness.finish()
        self.assertTrue(visible)
        self.verdict(trace, 0)

    def test_planted_missing_source_reference_is_reported(self):
        with DynamicHarness(self, async_mode=True, hold_source_ref=False) as harness:
            harness.put()
            self.assertTrue(harness.backend.posted.wait(TIMEOUT))
            harness.source.ref_count_down()  # page reclaimed while the write still reads it
            harness.release()
            trace, _ = harness.finish()
        self.assertIn('reclaim|page-', trace)
        self.verdict(trace, 1, 'premature_reclaim')

    def test_sync_put_is_clean(self):
        with DynamicHarness(self, async_mode=False) as harness:
            harness.put()
            self.assertTrue(harness.backend.posted.wait(TIMEOUT))
            harness.release()
            trace, visible = harness.finish()
        self.assertTrue(visible)
        self.assertIn('publish|', trace)
        self.verdict(trace, 0)

    def test_failed_write_without_quiescence_proof_is_inconclusive(self):
        with DynamicHarness(self, async_mode=True, outcome='ERR') as harness:
            harness.put()
            harness.release()
            trace, visible = harness.finish()
        self.assertFalse(visible)
        self.assertNotIn('publish|', trace)
        self.assertIn('coverage_gap|unproven-failed-transfer-completion', trace)
        self.assertNotIn('reclaim|object-', trace)  # write lease may still be live
        self.verdict(trace, 2, 'unproven-failed-transfer-completion')

    def test_failed_write_with_quiescent_mock_rolls_back_cleanly(self):
        for options in ({'async_mode': True, 'outcome': 'ERR'}, {'async_mode': True, 'post_error': True},
                        {'async_mode': False, 'outcome': 'ERR'}):
            with self.subTest(**options), DynamicHarness(self, quiescent=True, **options) as harness:
                harness.put()
                harness.release()
                trace, visible = harness.finish()
                self.assertFalse(visible)
                self.assertNotIn('publish|', trace)
                self.assertIn('complete|transfer-', trace)
                self.assertIn('reclaim|object-', trace)
                self.verdict(trace, 0)

    def test_failed_write_left_in_progress_is_inconclusive(self):
        # The earlier pin's async failure path never discards the progress entry.
        with DynamicHarness(self, async_mode=True, outcome='ERR', discard_on_failure=False,
                            quiescent=True) as harness:
            harness.put()
            harness.release()
            self.assertTrue(harness.backend.exists_in_put_tasks('key'))
            trace, visible = harness.finish()
        self.assertFalse(visible)
        self.verdict(trace, 2, 'cleanup-', 'no observed access completion')

    def test_acquisition_failure_posts_nothing(self):
        with DynamicHarness(self, async_mode=True, acquire_error=True) as harness:
            harness.put()
            harness.release()
            trace, visible = harness.finish()
        self.assertFalse(visible)
        self.assertNotIn('object-', trace)
        self.assertNotIn('submit|transfer-', trace)
        self.assertIn('complete|put-', trace)
        self.verdict(trace, 0)

    def test_remove_reclaims_published_object(self):
        with DynamicHarness(self, async_mode=True) as harness:
            harness.put()
            harness.release()
            self.assertTrue(harness.backend.remove('key'))
            trace, visible = harness.finish()
        self.assertFalse(visible)
        self.assertIn('reclaim|object-', trace)
        self.verdict(trace, 0)

    def test_hooks_restored(self):
        harness = DynamicHarness(self, async_mode=True)
        with harness:
            pass
        backend = harness.backend
        for obj, name in ((backend, '_acquire_storage_handle'), (backend, 'remove'),
                          (backend, 'mem_to_storage'), (backend.agent, 'post_async'),
                          (backend.agent.nixl_agent, 'check_xfer_state')):
            self.assertNotIn(name, obj.__dict__)
        self.assertIs(type(backend.progress_set), set)

    def test_put_path_validated(self):
        with self.assertRaises(ValueError):
            Capture(None, TraceWriter(), id, put_path='other')


class QuiescenceTests(unittest.TestCase):
    def backend(self):
        return static_backend(lambda backend, key, index: None)

    def test_flag_refused_without_transport_acknowledgement(self):
        for flags in ({'failed_post_is_quiescent': True},
                      {'failed_post_is_quiescent': True, 'quiescent_transport': True},
                      {'failed_post_is_quiescent': True, 'quiescent_transport': ''},
                      {'failed_post_is_quiescent': True, 'quiescent_transport': 'a|b'},
                      {'failed_post_is_quiescent': 1, 'quiescent_transport': 'mock'},
                      {'quiescent_transport': 'mock'}):
            with self.subTest(**flags):
                backend = self.backend()
                with self.assertRaises(ValueError):
                    instrument(backend, TraceWriter(), **flags)
                self.assertIs(type(backend.progress_set), set)  # nothing installed
            with self.subTest(capture=True, **flags), self.assertRaises(ValueError):
                Capture(None, TraceWriter(), id, flags.get('failed_post_is_quiescent', False),
                        flags.get('quiescent_transport'))

    def test_flag_enabled_with_named_transport(self):
        with instrument(self.backend(), TraceWriter(), failed_post_is_quiescent=True,
                        quiescent_transport='mock') as capture:
            self.assertTrue(capture.failed_post_is_quiescent)
            self.assertEqual(capture.quiescent_transport, 'mock')

    def test_static_failure_paths(self):
        for quiescent, expected in ((False, 'coverage_gap|unproven-failed-transfer-completion'),
                                    (True, 'complete|transfer-1|failed')):
            backend = self.backend()
            def fail(handle):
                raise RuntimeError('injected transfer failure')
            backend.agent.post_blocking = fail
            flags = {'failed_post_is_quiescent': True, 'quiescent_transport': 'mock'} if quiescent else {}
            writer = TraceWriter()
            with instrument(backend, writer, **flags) as capture:
                capture.current.set({'hold': 'put-1', 'sources': [], 'slots': [], 'failed': False})
                writer.submit('put-1', 'put')
                with self.assertRaises(RuntimeError):
                    backend.agent.post_blocking(object())
            self.assertIn(expected, writer.end())


class TimestampTests(unittest.TestCase):
    def test_clock_option_creates_timed_writer(self):
        with DynamicHarness(self, async_mode=True, clock=True) as harness:
            self.assertEqual(harness.writer.clock_name, 'monotonic')
            harness.put()
            harness.release()
            trace, _ = harness.finish()
        lines = trace.splitlines()
        self.assertTrue(lines[1].startswith('time|monotonic|'))
        readings = [int(line.split('|')[2]) for line in lines if line.startswith('time|')]
        self.assertEqual(readings, sorted(readings))
        if CHECKER:
            report = stateguard_trace.check(trace, checker=str(CHECKER))
            self.assertEqual(report['exit_code'], 0, report)
            timeline = stateguard_trace.timeline(trace, tool=str(CHECKER.parent / 'stateguard-timeline'))
            self.assertEqual(timeline['status'], 'complete', timeline)
            self.assertEqual(timeline['incomplete'], [])
            self.assertEqual(timeline['clock'], 'monotonic')
            self.assertLess(timeline['first_ns'], timeline['last_ns'])

    def test_clock_option_rejects_untimed_writer(self):
        backend = static_backend(lambda backend, key, index: None)
        with self.assertRaises(ValueError):
            instrument(backend, TraceWriter(), clock=True)
        with self.assertRaises(ValueError):
            instrument(backend, None, clock='yes')
        with instrument(backend, TraceWriter(clock=True), clock=True) as capture:
            self.assertEqual(capture.writer.clock_name, 'monotonic')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--checker', type=Path)
    args, rest = parser.parse_known_args()
    CHECKER = args.checker.resolve() if args.checker else None
    unittest.main(argv=['test_adapter', *rest])
