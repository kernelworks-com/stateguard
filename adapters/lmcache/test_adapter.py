"""Dependency-free capture checks; CLI conformance checks use --checker."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile
import unittest

from .trace import TraceWriter, identifier, uint
from .adapter import Capture, _Progress, instrument
from types import SimpleNamespace

CHECKER = None


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
        writer.finish()
        with self.assertRaises(RuntimeError):
            writer.allocate('page', 1, 4, 'cpu')

    def test_overflow(self):
        writer = TraceWriter(1)
        writer.allocate('page', 1, 4, 'cpu')
        for _ in range(100):
            writer.lifecycle('reclaim', ('page', 1))
        trace = writer.finish()
        self.assertEqual(len(trace.splitlines()), 4)
        self.assertEqual(trace.count('coverage_gap'), 1)
        self.assertTrue(trace.endswith('end|incomplete\n'))
        if CHECKER:
            self.assertEqual(self.report(trace).returncode, 2)

    def test_zero_capacity_and_record_bound(self):
        for writer in (TraceWriter(0), TraceWriter(10, 64)):
            writer.allocate('a' * 65, 1, 0, 'cpu')
            self.assertIn('coverage_gap|capture-overflow', writer.finish())

    def test_concurrent_transactions(self):
        writer = TraceWriter(4000)
        def transaction(n):
            with writer.lock:
                name = f'page-{n}'
                writer.allocate(name, 1, 32, 'cpu')
                writer.lifecycle('retire', (name, 1))
                writer.lifecycle('reclaim', (name, 1))
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(transaction, range(1000)))
        trace = writer.finish()
        self.assertEqual(len(trace.splitlines()), 3002)
        if CHECKER:
            result = self.report(trace)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_wire_format(self):
        writer = TraceWriter()
        writer.allocate('page', 1, 32, 'cpu')
        writer.submit('copy', [('r', ('page', 1))])
        writer.complete('copy', True)
        writer.lifecycle('retire', ('page', 1))
        writer.lifecycle('reclaim', ('page', 1))
        self.assertEqual(writer.finish(), 'STATEGUARD_TRACE|1\nallocate|page|1|32|cpu\n'
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
        trace = writer.finish()
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
        trace = writer.finish()
        self.assertIn('allocate|slot-1|2|0|storage', trace)
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
        for kind, fields in [('allocate', ('page', '0', '4', 'cpu')),
                             ('submit', ('op', 'req', 'r:bad:ref:1', '')),
                             ('publish', ('state', 'bad,ref', '1', '-')),
                             ('complete', ('op', 'unknown'))]:
            with self.assertRaises(ValueError):
                writer.emit(kind, *fields)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--checker', type=Path)
    args, rest = parser.parse_known_args()
    CHECKER = args.checker.resolve() if args.checker else None
    unittest.main(argv=['test_adapter', *rest])
