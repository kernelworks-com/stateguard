"""Writer output, validation, bounds, clocks, and the file writer."""
import io
import os
from pathlib import Path
import re
import tempfile
import threading
import unittest

import _support
from stateguard_trace import (Descriptor, FileTraceWriter, Route, TraceWriter, __version__,
                              identifier, text, uint)

DESCRIPTOR = Descriptor('demo', 'model-r1', 'cpu-cache', 'layer-0', 'prefix-42', 0, 16,
                        'fp16', 'row-major', 'absolute')
DESCRIPTOR_FIELDS = 'demo|model-r1|cpu-cache|layer-0|prefix-42|0|16|fp16|row-major|absolute'
ROUTE = Route('peer-a', 1, '7')

# One call per event kind and form, with the line the specification shows for
# it. Every expected line occurs verbatim in docs/TRACE-FORMAT.md.
SPEC_LINES = [
    (lambda w: w.allocate('buffer-1', 1, 4096, 'cpu'), 'allocate|buffer-1|1|4096|cpu'),
    (lambda w: w.bind_state('buffer-1', 1, 'state-1', DESCRIPTOR),
     'bind_state|buffer-1|1|state-1|' + DESCRIPTOR_FIELDS),
    (lambda w: w.submit('copy-1', 'request-1', [('w', 'buffer-1', 1)]),
     'submit|copy-1|request-1|w:buffer-1:1|'),
    (lambda w: w.submit('load-1', 'loader', [('r', 'staging-1', 1), ('w', ('cache-1', 1))],
                        route=ROUTE),
     'submit|load-1|loader|r:staging-1:1;w:cache-1:1||peer-a:1:7'),
    (lambda w: w.publish('state-1', 'buffer-1', 1, 'copy-1'), 'publish|state-1|buffer-1|1|copy-1'),
    (lambda w: w.consume('request-1', 'state-1', 'buffer-1', 1, DESCRIPTOR),
     'consume|request-1|state-1|buffer-1|1|' + DESCRIPTOR_FIELDS),
    (lambda w: w.cancel('request-1'), 'cancel|request-1'),
    (lambda w: w.cancel_signal('request-1'), 'cancel_signal|request-1'),
    (lambda w: w.finish('request-2'), 'finish|request-2'),
    (lambda w: w.complete('copy-1'), 'complete|copy-1'),
    (lambda w: w.complete('op', 'succeeded'), 'complete|op'),
    (lambda w: w.complete('op', 'failed'), 'complete|op|failed'),
    (lambda w: w.complete('load-1', evidence=ROUTE), 'complete|load-1|succeeded|peer-a:1:7'),
    (lambda w: w.complete('op', 'failed', ('peer-a', 1, '7')), 'complete|op|failed|peer-a:1:7'),
    (lambda w: w.connect('peer-a', 2), 'connect|peer-a|2'),
    (lambda w: w.expire('load-1', 'timeout'), 'expire|load-1|timeout'),
    (lambda w: w.retire('buffer-1', 1), 'retire|buffer-1|1'),
    (lambda w: w.reclaim('buffer-1', 1), 'reclaim|buffer-1|1'),
    (lambda w: w.coverage_gap('capture-overflow'), 'coverage_gap|capture-overflow'),
    (lambda w: w.time('steady', 1000), 'time|steady|1000'),
]


def body(writer):
    """The recorded lines between the header and the end marker."""
    return writer.getvalue().splitlines()[1:]


class EventLines(unittest.TestCase):
    def test_every_kind_matches_the_specification(self):
        spec = _support.SPEC.read_text(encoding='utf-8').replace('\\|', '|')
        spec_lines = set(re.findall(r'[a-z_]+\|[^`\n]*', spec))
        for call, expected in SPEC_LINES:
            with self.subTest(expected=expected):
                self.assertIn(expected, spec_lines)
                writer = TraceWriter()
                call(writer)
                self.assertEqual(body(writer), [expected])

    def test_end_markers(self):
        self.assertEqual(TraceWriter().end(), 'STATEGUARD_TRACE|1\nend|complete\n')
        self.assertEqual(TraceWriter().end(complete=False), 'STATEGUARD_TRACE|1\nend|incomplete\n')

    def test_forms_without_a_spec_line(self):
        writer = TraceWriter()
        writer.publish('state-1', 'buffer-1', 1)
        writer.submit('copy-2', 'request-1', [], ['copy-1', 'load-1'])
        writer.submit('copy-3', 'request-1')
        self.assertEqual(body(writer), ['publish|state-1|buffer-1|1|-',
                                        'submit|copy-2|request-1||copy-1,load-1',
                                        'submit|copy-3|request-1||'])

    def test_text_fields_allow_list_separators(self):
        writer = TraceWriter()
        descriptor = DESCRIPTOR._replace(prefix='a;b:c,d e')
        writer.bind_state('buffer-1', 1, 'state-1', descriptor)
        self.assertIn('|a;b:c,d e|', writer.getvalue())

    def test_no_clock_output_is_unchanged(self):
        # The exact bytes the writer produced before clocks existed.
        writer = TraceWriter()
        writer.allocate('page', 1, 32, 'cpu')
        writer.submit('copy', 'put', [('r', ('page', 1))])
        writer.complete('copy', 'failed')
        writer.retire('page', 1)
        writer.reclaim('page', 1)
        self.assertEqual(writer.end(), 'STATEGUARD_TRACE|1\nallocate|page|1|32|cpu\n'
                         'submit|copy|put|r:page:1|\ncomplete|copy|failed\n'
                         'retire|page|1\nreclaim|page|1\nend|complete\n')

    def test_version(self):
        self.assertEqual(__version__, '0.3.0')


class Validation(unittest.TestCase):
    def rejects(self, call, message):
        writer = TraceWriter()
        with self.assertRaisesRegex(ValueError, message):
            call(writer)
        self.assertEqual(body(writer), [], 'a rejected event must not be recorded')

    def test_identifier_reserved_characters(self):
        for c in '|;:,\n\r':
            with self.subTest(c=c):
                self.rejects(lambda w: w.allocate(f'a{c}b', 1, 1, 'cpu'), 'reserved character')
                self.rejects(lambda w: w.cancel(f'r{c}'), 'request .* reserved')
        self.rejects(lambda w: w.cancel(''), 'nonempty')
        self.rejects(lambda w: w.cancel(7), 'must be a str')

    def test_text_reserved_characters(self):
        for c in '|\n\r':
            bad = DESCRIPTOR._replace(layout=f'row{c}major')
            self.rejects(lambda w: w.bind_state('b', 1, 's', bad), 'descriptor layout')
        self.rejects(lambda w: w.consume('r', 's', 'b', 1, DESCRIPTOR._replace(adapter='')),
                     'nonempty')

    def test_integers(self):
        for value in (-1, 1 << 64, True, 1.0, '1'):
            self.rejects(lambda w: w.allocate('b', 1, value, 'cpu'), 'bytes')
        self.rejects(lambda w: w.allocate('b', 0, 1, 'cpu'), 'generation 0 is outside 1')
        self.rejects(lambda w: w.retire('b', 0), 'generation')
        self.rejects(lambda w: w.connect('peer', 0), 'epoch')
        self.rejects(lambda w: w.time('steady', -1), 'nanoseconds')
        writer = TraceWriter()
        writer.allocate('b', (1 << 64) - 1, (1 << 64) - 1, 'cpu')
        writer.time('steady', 0)
        self.assertEqual(body(writer)[0], 'allocate|b|18446744073709551615|18446744073709551615|cpu')

    def test_descriptor_shape(self):
        self.rejects(lambda w: w.bind_state('b', 1, 's', DESCRIPTOR[:9]), '10 fields')
        self.rejects(lambda w: w.bind_state('b', 1, 's', 'x' * 10), '10 fields')
        self.rejects(lambda w: w.bind_state('b', 1, 's', DESCRIPTOR._replace(token_begin=17)),
                     'less than token_begin')
        self.rejects(lambda w: w.bind_state('b', 1, 's', DESCRIPTOR._replace(token_end='16')),
                     'token_end must be an int')

    def test_submit_fields(self):
        self.rejects(lambda w: w.submit('op', 'r', [('x', 'b', 1)]), "access must be 'r' or 'w'")
        self.rejects(lambda w: w.submit('op', 'r', [('r', 'b', 0)]), 'lease generation')
        self.rejects(lambda w: w.submit('op', 'r', [('r', 'b:c', 1)]), 'lease allocation')
        self.rejects(lambda w: w.submit('op', 'r', 'r:b:1'), 'not strings')
        self.rejects(lambda w: w.submit('op', 'r', [], ['a,b']), 'dependency')
        self.rejects(lambda w: w.submit('op', 'r', [], [], ('peer', 0, '1')), 'route epoch')
        self.rejects(lambda w: w.submit('op', 'r', [], [], 'peer:1:1'), 'route must be')
        self.rejects(lambda w: w.submit('op', 'r', [], [], ('peer', 1, 'a:b')), 'route tag')

    def test_completion_and_publication(self):
        self.rejects(lambda w: w.complete('op', 'unknown'), 'outcome')
        self.rejects(lambda w: w.complete('op', evidence=('peer', 1)), 'evidence must be')
        self.rejects(lambda w: w.publish('s', 'b', 1, '-'), 'reserved')

    def test_helpers(self):
        self.assertEqual(identifier('page-1'), 'page-1')
        self.assertEqual(text('a;b'), 'a;b')
        self.assertEqual(uint(0), '0')
        with self.assertRaises(ValueError):
            uint(0, nonzero=True)


class Lifecycle(unittest.TestCase):
    def test_ended_writer_rejects_events(self):
        writer = TraceWriter()
        writer.end()
        self.assertTrue(writer.ended)
        with self.assertRaises(RuntimeError):
            writer.cancel('r')
        with self.assertRaises(RuntimeError):
            writer.end()

    def test_context_manager_writes_the_end_marker(self):
        with TraceWriter() as writer:
            writer.cancel('r')
        self.assertTrue(writer.getvalue().endswith('cancel|r\nend|complete\n'))
        with self.assertRaises(KeyError):
            with TraceWriter() as failed:
                failed.cancel('r')
                raise KeyError('stop')
        self.assertTrue(failed.getvalue().endswith('cancel|r\nend|incomplete\n'))
        with TraceWriter() as early:
            early.end(complete=False)
        self.assertEqual(early.getvalue().count('end|'), 1)

    def test_capacity_overflow(self):
        writer = TraceWriter(1)
        writer.allocate('page', 1, 4, 'cpu')
        for _ in range(10):
            writer.reclaim('page', 1)
        self.assertTrue(writer.lost)
        self.assertEqual(writer.end(), 'STATEGUARD_TRACE|1\nallocate|page|1|4|cpu\n'
                         'coverage_gap|capture-overflow\nend|incomplete\n')

    def test_record_bound_and_unbounded(self):
        writer = TraceWriter(10, 64)
        writer.allocate('a' * 65, 1, 0, 'cpu')
        writer.cancel('r')
        self.assertEqual(body(writer), [])
        self.assertIn('coverage_gap|capture-overflow\nend|incomplete', writer.end())
        unbounded = TraceWriter(None, None)
        unbounded.allocate('a' * 5000, 1, 0, 'cpu')
        self.assertFalse(unbounded.lost)

    def test_bad_bounds(self):
        for args in ((-1,), (1.5,), (10, 63)):
            with self.assertRaises(ValueError):
                TraceWriter(*args)

    def test_threads(self):
        writer = TraceWriter(None)
        def work(n):
            for i in range(100):
                with writer.lock:
                    writer.allocate(f'b-{n}-{i}', 1, 1, 'cpu')
                    writer.reclaim(f'b-{n}-{i}', 1)
        threads = [threading.Thread(target=work, args=(n,)) for n in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        lines = body(writer)
        self.assertEqual(len(lines), 1600)
        for first, second in zip(lines[::2], lines[1::2]):
            self.assertEqual(first.split('|')[1], second.split('|')[1])


class Clock(unittest.TestCase):
    @staticmethod
    def readings(*values):
        it = iter(values)
        return lambda: next(it)

    def test_time_line_on_each_new_reading(self):
        writer = TraceWriter(clock=self.readings(100, 100, 250, 250), clock_name='test')
        self.assertEqual(writer.clock_name, 'test')
        writer.allocate('b', 1, 1, 'cpu')
        writer.cancel_signal('r')
        writer.cancel('r')
        self.assertEqual(writer.end(), 'STATEGUARD_TRACE|1\ntime|test|100\nallocate|b|1|1|cpu\n'
                         'cancel_signal|r\ntime|test|250\ncancel|r\nend|complete\n')

    def test_default_clock_is_monotonic(self):
        writer = TraceWriter(clock=True)
        writer.cancel('r')
        lines = body(writer)
        self.assertEqual(writer.clock_name, 'monotonic')
        self.assertRegex(lines[0], r'^time\|monotonic\|\d+$')
        self.assertEqual(lines[1], 'cancel|r')

    def test_readings_must_not_decrease(self):
        writer = TraceWriter(clock=self.readings(200, 100))
        writer.cancel('a')
        with self.assertRaisesRegex(ValueError, 'less than the previous'):
            writer.cancel('b')
        self.assertEqual(body(writer), ['time|monotonic|200', 'cancel|a'])
        for bad in (-1, 1.5, None):
            with self.assertRaisesRegex(ValueError, 'clock reading'):
                TraceWriter(clock=lambda: bad).cancel('r')

    def test_time_and_event_are_kept_or_dropped_together(self):
        writer = TraceWriter(3, clock=self.readings(1, 2, 3, 4, 4))
        writer.cancel('a')   # time + event: 2 records
        writer.cancel('b')   # would need 2 more; both are dropped
        writer.cancel('c')
        self.assertEqual(writer.end(), 'STATEGUARD_TRACE|1\ntime|monotonic|1\ncancel|a\n'
                         'time|monotonic|4\ncoverage_gap|capture-overflow\nend|incomplete\n')

    def test_manual_time_rules(self):
        with self.assertRaisesRegex(ValueError, 'own clock'):
            TraceWriter(clock=True).time('monotonic', 1)
        writer = TraceWriter()
        writer.time('steady', 10)
        writer.time('steady', 10)
        with self.assertRaisesRegex(ValueError, 'less than the previous'):
            writer.time('steady', 9)
        with self.assertRaisesRegex(ValueError, 'differs from the trace clock'):
            writer.time('other', 20)
        self.assertEqual(body(writer), ['time|steady|10', 'time|steady|10'])

    def test_bad_clock(self):
        with self.assertRaises(ValueError):
            TraceWriter(clock=5)
        with self.assertRaises(ValueError):
            TraceWriter(clock=True, clock_name='a|b')


class FileWriter(unittest.TestCase):
    def test_path_target_streams_lf_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'out.sgtrace'
            with FileTraceWriter(path, flush=True) as writer:
                writer.allocate('b', 1, 1, 'cpu')
                self.assertEqual(path.read_bytes(),
                                 b'STATEGUARD_TRACE|1\nallocate|b|1|1|cpu\n')
            self.assertEqual(writer.path, os.fspath(path))
            self.assertEqual(path.read_bytes(),
                             b'STATEGUARD_TRACE|1\nallocate|b|1|1|cpu\nend|complete\n')
            self.assertTrue(writer._file.closed)

    def test_file_objects(self):
        binary = io.BytesIO()
        writer = FileTraceWriter(binary, capacity=1)
        writer.cancel('a')
        writer.cancel('b')
        writer.end()
        self.assertEqual(binary.getvalue(), b'STATEGUARD_TRACE|1\ncancel|a\n'
                         b'coverage_gap|capture-overflow\nend|incomplete\n')
        self.assertFalse(binary.closed)
        textual = io.StringIO(newline='\n')
        with FileTraceWriter(textual) as writer:
            writer.finish('r')
        self.assertEqual(textual.getvalue(), 'STATEGUARD_TRACE|1\nfinish|r\nend|complete\n')


if __name__ == '__main__':
    unittest.main()
