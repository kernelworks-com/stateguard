"""Every spec example and fixture is reproduced byte for byte from typed calls.

Each line is decoded into Python values (ints, tuples, None), not strings
passed through, and replayed through the writer's public methods. A trace the
writer could not express would fail here; at present none is excluded.
"""
import io
import unittest

import _support
from stateguard_trace import Descriptor, FileTraceWriter, Route, TraceWriter


def ref(fields):
    return fields[0], int(fields[1])


def route(field):
    endpoint, epoch, tag = field.split(':')
    return Route(endpoint, int(epoch), tag)


def descriptor(fields):
    values = list(fields)
    values[5], values[6] = int(values[5]), int(values[6])
    return Descriptor(*values)


def call(line):
    """Decode one event line into (method name, positional args, keyword args)."""
    kind, *f = line.split('|')
    if kind == 'allocate':
        return kind, (*ref(f), int(f[2]), f[3]), {}
    if kind == 'bind_state':
        return kind, (*ref(f), f[2], descriptor(f[3:])), {}
    if kind == 'submit':
        leases = [(a, n, int(g)) for a, n, g in (x.split(':') for x in f[2].split(';') if x)]
        dependencies = [d for d in f[3].split(',') if d]
        return kind, (f[0], f[1], leases, dependencies), {'route': route(f[4]) if len(f) == 5 else None}
    if kind == 'publish':
        return kind, (f[0], *ref(f[1:]), None if f[3] == '-' else f[3]), {}
    if kind == 'consume':
        return kind, (f[0], f[1], *ref(f[2:]), descriptor(f[4:])), {}
    if kind == 'complete':
        outcome = f[1] if len(f) > 1 else 'succeeded'
        return kind, (f[0], outcome), {'evidence': route(f[2]) if len(f) == 3 else None}
    if kind in ('connect', 'time'):
        return kind, (f[0], int(f[1])), {}
    if kind in ('retire', 'reclaim'):
        return kind, ref(f), {}
    if kind == 'end':
        return kind, (), {'complete': {'complete': True, 'incomplete': False}[f[0]]}
    return kind, tuple(f), {}   # cancel, cancel_signal, finish, expire, coverage_gap


def replay(trace, writer):
    lines = trace.split('\n')
    assert lines[0] == 'STATEGUARD_TRACE|1' and lines[-1] == '', 'not a canonical trace'
    for line in lines[1:-1]:
        name, args, kwargs = call(line)
        getattr(writer, name)(*args, **kwargs)
    return writer


def corpus():
    yield from _support.spec_traces()
    for path in sorted(_support.FIXTURES.glob('*.sgtrace')):
        # Universal newlines: a Windows checkout may store fixtures with CRLF.
        yield path.name, path.read_text(encoding='utf-8')


class Conformance(unittest.TestCase):
    def test_corpus_is_present(self):
        names = [name for name, _ in corpus()]
        self.assertGreaterEqual(sum(n.startswith('TRACE-FORMAT') for n in names), 3)
        self.assertGreaterEqual(sum(n.endswith('.sgtrace') for n in names), 15)

    def test_memory_writer_reproduces_every_trace(self):
        for name, trace in corpus():
            with self.subTest(trace=name):
                # The header example (a bare header line) has no end marker.
                writer = replay(trace, TraceWriter(None, None))
                self.assertEqual(writer.ended, 'end|' in trace)
                self.assertEqual(writer.getvalue(), trace)

    def test_file_writer_reproduces_every_trace(self):
        for name, trace in corpus():
            with self.subTest(trace=name):
                sink = io.BytesIO()
                replay(trace, FileTraceWriter(sink))
                self.assertEqual(sink.getvalue(), trace.encode('utf-8'))


if __name__ == '__main__':
    unittest.main()
