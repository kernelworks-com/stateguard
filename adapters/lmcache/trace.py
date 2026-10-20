"""Bounded schema-1 trace capture; no LMCache or torch dependency."""
from threading import RLock

MAX_UINT = (1 << 64) - 1


def identifier(value):
    if not isinstance(value, str) or not value or any(c in value for c in '|;:,\n\r'):
        raise ValueError('invalid trace identifier')
    return value


def uint(value, nonzero=False):
    if type(value) is not int or not (int(nonzero) <= value <= MAX_UINT):
        raise ValueError('invalid trace integer')
    return str(value)


class TraceWriter:
    """Capacity counts events, excluding the reserved gap and end markers.

    A record has at most max_record_bytes UTF-8 bytes. Lock order defines the
    observed order within one process; it does not infer device happens-before.
    """
    def __init__(self, capacity=100000, max_record_bytes=4096):
        if type(capacity) is not int or capacity < 0:
            raise ValueError('invalid capacity')
        if type(max_record_bytes) is not int or max_record_bytes < 64:
            raise ValueError('invalid record bound')
        self.lock = RLock()
        self.capacity = capacity
        self.max_record_bytes = max_record_bytes
        self._events = []
        self._lost = False
        self._closed = False

    def emit(self, kind, *fields):
        counts = {'allocate': (4,), 'bind_state': (13,), 'submit': (4,),
                  'publish': (4,), 'consume': (14,), 'complete': (1, 2),
                  'retire': (2,), 'reclaim': (2,), 'coverage_gap': (1,)}
        if kind not in counts or len(fields) not in counts[kind]:
            raise ValueError('unsupported event or field count')
        if any(not isinstance(f, str) or any(c in f for c in '|\n\r') for f in fields):
            raise ValueError('invalid pipe field')
        def number(text, nonzero=False):
            if not text.isascii() or not text.isdecimal():
                raise ValueError('invalid integer field')
            uint(int(text), nonzero)
        if kind == 'submit':
            identifier(fields[0])
            identifier(fields[1])
            if fields[2]:
                for lease in fields[2].split(';'):
                    parts = lease.split(':')
                    if len(parts) != 3 or parts[0] not in ('r', 'w'):
                        raise ValueError('invalid lease')
                    identifier(parts[1])
                    number(parts[2], True)
            if fields[3]:
                for dependency in fields[3].split(','):
                    identifier(dependency)
        elif kind in ('bind_state', 'consume'):
            offset = 3 if kind == 'bind_state' else 4
            for text in fields[:offset]:
                identifier(text)
            number(fields[offset - 2 if kind == 'bind_state' else offset - 1], True)
            descriptor = fields[offset:]
            if any(not text for text in descriptor):
                raise ValueError('empty descriptor field')
            number(descriptor[5])
            number(descriptor[6])
            if int(descriptor[5]) > int(descriptor[6]):
                raise ValueError('inverted token interval')
        else:
            identifier(fields[0])
            if kind in ('allocate', 'retire', 'reclaim'):
                number(fields[1], True)
            if kind == 'allocate':
                number(fields[2])
                identifier(fields[3])
            if kind == 'publish':
                identifier(fields[1])
                number(fields[2], True)
                identifier(fields[3])
            if kind == 'complete' and len(fields) == 2 and fields[1] not in ('succeeded', 'failed'):
                raise ValueError('invalid completion outcome')
        line = '|'.join((kind, *fields))
        if '\n' in line or '\r' in line:
            raise ValueError('multiline event')
        with self.lock:
            if self._closed:
                raise RuntimeError('capture is closed')
            if self._lost:
                return
            if len(self._events) >= self.capacity or len(line.encode('utf-8')) > self.max_record_bytes:
                self._lost = True
                return
            self._events.append(line)

    def allocate(self, allocation, generation, size, domain):
        self.emit('allocate', identifier(allocation), uint(generation, True), uint(size), identifier(domain))

    def lifecycle(self, kind, ref):
        if kind not in ('retire', 'reclaim'):
            raise ValueError('invalid lifecycle event')
        self.emit(kind, identifier(ref[0]), uint(ref[1], True))

    def submit(self, operation, leases):
        encoded = []
        for access, ref in leases:
            if access not in ('r', 'w'):
                raise ValueError('invalid lease access')
            encoded.append(':'.join((access, identifier(ref[0]), uint(ref[1], True))))
        self.emit('submit', identifier(operation), 'put', ';'.join(encoded), '')

    def complete(self, operation, failed=False):
        self.emit('complete', identifier(operation), *(['failed'] if failed else []))

    def finish(self, complete=True):
        with self.lock:
            self._closed = True
            tail = ['coverage_gap|capture-overflow'] if self._lost else []
            return '\n'.join(['STATEGUARD_TRACE|1', *self._events, *tail,
                              'end|' + ('complete' if complete and not self._lost else 'incomplete'), ''])
