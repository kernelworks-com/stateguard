"""Schema-1 `.sgtrace` writers.

Every method validates its values as docs/TRACE-FORMAT.md requires and raises
ValueError before anything is recorded. The output is the canonical form that
`serialize_trace` in src/trace.cpp writes.
"""
from collections import namedtuple
import io
import os
from threading import RLock
import time as _time

HEADER = 'STATEGUARD_TRACE|1'
MAX_UINT = (1 << 64) - 1
OVERFLOW_REASON = 'capture-overflow'

_IDENTIFIER_RESERVED = '|;:,\n\r'
_TEXT_RESERVED = '|\n\r'

Descriptor = namedtuple('Descriptor', (
    'namespace', 'model_revision', 'adapter', 'component', 'prefix',
    'token_begin', 'token_end', 'representation', 'layout', 'position'))
Descriptor.__doc__ = 'The ten fields of a semantic descriptor, in trace order.'

Route = namedtuple('Route', ('endpoint', 'epoch', 'tag'))
Route.__doc__ = 'A transfer route: endpoint, connection epoch, backend tag.'


def _reserved(value, reserved, what):
    if not isinstance(value, str):
        raise ValueError(f'{what} must be a str, not {type(value).__name__}')
    if not value:
        raise ValueError(f'{what} must be nonempty')
    for c in value:
        if c in reserved:
            raise ValueError(f'{what} {value!r} contains the reserved character {c!r}')
    return value


def identifier(value, what='identifier'):
    """Return value if it is a valid identifier, else raise ValueError."""
    return _reserved(value, _IDENTIFIER_RESERVED, what)


def text(value, what='text'):
    """Return value if it is a valid descriptor text field, else raise ValueError."""
    return _reserved(value, _TEXT_RESERVED, what)


def uint(value, nonzero=False, what='integer'):
    """Return the decimal form of an unsigned 64-bit int, else raise ValueError."""
    if type(value) is not int:
        raise ValueError(f'{what} must be an int, not {type(value).__name__}')
    if not (int(nonzero) <= value <= MAX_UINT):
        bound = '1' if nonzero else '0'
        raise ValueError(f'{what} {value} is outside {bound} to 2^64 - 1')
    return str(value)


def _ref(allocation, generation, what='allocation'):
    return (identifier(allocation, what),
            uint(generation, True, what.replace('allocation', 'generation')))


def _descriptor(value):
    if isinstance(value, (str, bytes)) or len(value) != 10:
        raise ValueError('descriptor must have exactly 10 fields')
    fields = []
    for name, field in zip(Descriptor._fields, value):
        if name in ('token_begin', 'token_end'):
            fields.append(uint(field, what=f'descriptor {name}'))
        else:
            fields.append(text(field, f'descriptor {name}'))
    if int(fields[5]) > int(fields[6]):
        raise ValueError(f'descriptor token_end {fields[6]} is less than token_begin {fields[5]}')
    return fields


def _route(value, what='route'):
    if isinstance(value, (str, bytes)) or len(value) != 3:
        raise ValueError(f'{what} must be (endpoint, epoch, tag)')
    endpoint, epoch, tag = value
    return ':'.join((identifier(endpoint, f'{what} endpoint'),
                     uint(epoch, True, f'{what} epoch'),
                     identifier(tag, f'{what} tag')))


def _lease(value):
    # (access, allocation, generation) or (access, (allocation, generation)).
    if isinstance(value, (str, bytes)) or len(value) not in (2, 3):
        raise ValueError('lease must be (access, allocation, generation)')
    if len(value) == 2:
        access, ref = value
        if isinstance(ref, (str, bytes)) or len(ref) != 2:
            raise ValueError('lease must be (access, allocation, generation)')
        allocation, generation = ref
    else:
        access, allocation, generation = value
    if access not in ('r', 'w'):
        raise ValueError(f"lease access must be 'r' or 'w', not {access!r}")
    return ':'.join((access, *_ref(allocation, generation, 'lease allocation')))


class _Writer:
    """Validation, ordering, bounds, and timestamps shared by both writers.

    capacity bounds the number of recorded lines after the header, including
    `time` lines and excluding the reserved overflow gap and end marker. A line
    longer than max_record_bytes UTF-8 bytes counts as overflow. On overflow
    that line and every later one are dropped, and end() writes one
    `coverage_gap|capture-overflow` and `end|incomplete`. None means unbounded.

    clock: None records no `time` lines. True uses time.monotonic_ns. Any
    other callable returning integer nanoseconds is used as the clock. With a
    clock, each record that is the first at a new reading is preceded by
    `time|<clock_name>|<ns>`; the pair is kept or dropped together. Readings
    must not decrease. Without a clock, time() records readings by hand, with
    the same one-clock, non-decreasing rule.

    `lock` is reentrant. Trace order is the order in which records take it, so
    hold it around the code that makes an effect visible to keep the order
    faithful across threads.
    """

    def __init__(self, capacity, max_record_bytes, clock, clock_name):
        if capacity is not None and (type(capacity) is not int or capacity < 0):
            raise ValueError('capacity must be a nonnegative int or None')
        if max_record_bytes is not None and (type(max_record_bytes) is not int or max_record_bytes < 64):
            raise ValueError('max_record_bytes must be an int of at least 64, or None')
        if clock is True:
            clock = _time.monotonic_ns
        if clock is not None and not callable(clock):
            raise ValueError('clock must be None, True, or a callable returning nanoseconds')
        self.lock = RLock()
        self.capacity = capacity
        self.max_record_bytes = max_record_bytes
        self._clock = clock
        self._clock_name = identifier(clock_name, 'clock name') if clock else None
        self._last_ns = None
        self._recorded = 0
        self._lost = False
        self._ended = False

    # Bookkeeping ---------------------------------------------------------

    @property
    def lost(self):
        """True once a record was dropped by the capacity or record bound."""
        return self._lost

    @property
    def ended(self):
        return self._ended

    @property
    def clock_name(self):
        return self._clock_name

    def _sink(self, lines):
        raise NotImplementedError

    def _stamp(self):
        reading = self._clock()
        uint(reading, what='clock reading')
        if self._last_ns is not None and reading < self._last_ns:
            raise ValueError(f'clock reading {reading} is less than the previous {self._last_ns}')
        if reading == self._last_ns:
            return []
        return [(f'time|{self._clock_name}|{reading}', reading)]

    def _record(self, *fields, reserved=False):
        line = '|'.join(fields)
        with self.lock:
            if self._ended:
                raise RuntimeError('trace already ended')
            pending = self._stamp() if self._clock else []
            if self._lost and not reserved:
                return
            pending.append((line, None))
            if not reserved:
                too_many = (self.capacity is not None
                            and self._recorded + len(pending) > self.capacity)
                too_long = (self.max_record_bytes is not None
                            and any(len(text.encode('utf-8')) > self.max_record_bytes
                                    for text, _ in pending))
                if too_many or too_long:
                    self._lost = True
                    return
                self._recorded += len(pending)
            for _, reading in pending:
                if reading is not None:
                    self._last_ns = reading
            self._sink([text for text, _ in pending])

    # Events, in the order of the specification's event table -------------

    def allocate(self, allocation, generation, size, domain):
        """`allocate`: the allocation generation's memory becomes live."""
        self._record('allocate', *_ref(allocation, generation), uint(size, what='bytes'),
                     identifier(domain, 'domain'))

    def bind_state(self, allocation, generation, state, descriptor):
        """`bind_state`: the generation holds state, described by a 10-field descriptor."""
        self._record('bind_state', *_ref(allocation, generation), identifier(state, 'state'),
                     *_descriptor(descriptor))

    def submit(self, operation, request, leases=(), dependencies=(), route=None):
        """`submit`: an operation started; leases are (access, allocation, generation)."""
        if isinstance(leases, (str, bytes)) or isinstance(dependencies, (str, bytes)):
            raise ValueError('leases and dependencies must be sequences, not strings')
        fields = ['submit', identifier(operation, 'operation'), identifier(request, 'request'),
                  ';'.join(_lease(lease) for lease in leases),
                  ','.join(identifier(d, 'dependency') for d in dependencies)]
        if route is not None:
            fields.append(_route(route))
        self._record(*fields)

    def publish(self, state, allocation, generation, operation=None):
        """`publish`: state becomes visible; operation is the completed writer, or None."""
        if operation == '-':
            raise ValueError("publish operation '-' is reserved; pass None for no proof")
        self._record('publish', identifier(state, 'state'), *_ref(allocation, generation),
                     '-' if operation is None else identifier(operation, 'operation'))

    def consume(self, request, state, allocation, generation, descriptor):
        """`consume`: a request reads state and expects the descriptor."""
        self._record('consume', identifier(request, 'request'), identifier(state, 'state'),
                     *_ref(allocation, generation), *_descriptor(descriptor))

    def cancel(self, request):
        """`cancel`: the scheduler acknowledged the cancellation."""
        self._record('cancel', identifier(request, 'request'))

    def cancel_signal(self, request):
        """`cancel_signal`: the process received a cancellation signal."""
        self._record('cancel_signal', identifier(request, 'request'))

    def finish(self, request):
        """`finish`: the request ended normally. To end the trace, use end()."""
        self._record('finish', identifier(request, 'request'))

    def complete(self, operation, outcome='succeeded', evidence=None):
        """`complete`: the operation's access ended.

        outcome is 'succeeded' or 'failed'. evidence is the route named by the
        backend's completion notification. A success without evidence is
        written in the two-field form, as the reference serializer does.
        """
        if outcome not in ('succeeded', 'failed'):
            raise ValueError(f"completion outcome must be 'succeeded' or 'failed', not {outcome!r}")
        fields = ['complete', identifier(operation, 'operation')]
        if outcome == 'failed' or evidence is not None:
            fields.append(outcome)
        if evidence is not None:
            fields.append(_route(evidence, 'evidence'))
        self._record(*fields)

    def connect(self, endpoint, epoch):
        """`connect`: the endpoint started a connection epoch."""
        self._record('connect', identifier(endpoint, 'endpoint'), uint(epoch, True, 'epoch'))

    def expire(self, operation, reason):
        """`expire`: the host stopped waiting; the leases stay held."""
        self._record('expire', identifier(operation, 'operation'), identifier(reason, 'reason'))

    def retire(self, allocation, generation):
        """`retire`: the generation takes no new users."""
        self._record('retire', *_ref(allocation, generation))

    def reclaim(self, allocation, generation):
        """`reclaim`: the generation returned to its pool."""
        self._record('reclaim', *_ref(allocation, generation))

    def coverage_gap(self, reason):
        """`coverage_gap`: events may be missing here. End such a trace incomplete."""
        self._record('coverage_gap', identifier(reason, 'reason'))

    def time(self, clock, nanoseconds):
        """`time`: record a reading by hand. Not allowed when the writer has a clock."""
        identifier(clock, 'clock')
        uint(nanoseconds, what='nanoseconds')
        with self.lock:
            if self._clock:
                raise ValueError('this writer stamps time lines from its own clock')
            if self._clock_name is not None and clock != self._clock_name:
                raise ValueError(f'clock {clock!r} differs from the trace clock {self._clock_name!r}')
            if self._last_ns is not None and nanoseconds < self._last_ns:
                raise ValueError(f'time {nanoseconds} is less than the previous {self._last_ns}')
            self._record('time', clock, str(nanoseconds))
            if not self._lost:
                self._clock_name, self._last_ns = clock, nanoseconds

    def _end(self, complete):
        if type(complete) is not bool:
            raise ValueError('complete must be a bool')
        with self.lock:
            if self._ended:
                raise RuntimeError('trace already ended')
            if self._lost:
                self._record('coverage_gap', OVERFLOW_REASON, reserved=True)
            self._record('end', 'complete' if complete and not self._lost else 'incomplete',
                         reserved=True)
            self._ended = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        # An exception may have cut the capture short, so the trace cannot
        # claim it saw every event.
        with self.lock:
            if not self._ended:
                self.end(complete=exc_type is None)
        return False


class TraceWriter(_Writer):
    """Collects a trace in memory. end() returns the finished text."""

    def __init__(self, capacity=100000, max_record_bytes=4096, *, clock=None,
                 clock_name='monotonic'):
        super().__init__(capacity, max_record_bytes, clock, clock_name)
        self._lines = [HEADER]

    def _sink(self, lines):
        self._lines.extend(lines)

    def getvalue(self):
        """The trace so far, ending with a newline (and the end marker once ended)."""
        with self.lock:
            return '\n'.join(self._lines) + '\n'

    def end(self, complete=True):
        """Write the end marker (`incomplete` after overflow) and return the trace."""
        with self.lock:
            self._end(complete)
            return self.getvalue()


class FileTraceWriter(_Writer):
    """Writes a trace to a file as it is recorded.

    target is a path, or an open file object. A text file must have been
    opened with newline='\\n' (or '') so that lines end with LF on every
    platform; a binary file receives UTF-8 bytes. The writer closes a file it
    opened itself when the trace ends. Unbounded by default.
    """

    def __init__(self, target, capacity=None, max_record_bytes=None, *, clock=None,
                 clock_name='monotonic', flush=False):
        super().__init__(capacity, max_record_bytes, clock, clock_name)
        self._owned = isinstance(target, (str, bytes, os.PathLike))
        self._file = open(target, 'wb') if self._owned else target
        self._binary = not isinstance(self._file, io.TextIOBase)
        self._flush = flush
        self.path = os.fspath(target) if self._owned else getattr(target, 'name', None)
        self._sink([HEADER])

    def _sink(self, lines):
        data = ''.join(line + '\n' for line in lines)
        self._file.write(data.encode('utf-8') if self._binary else data)
        if self._flush:
            self._file.flush()

    def flush(self):
        with self.lock:
            self._file.flush()

    def end(self, complete=True):
        """Write the end marker (`incomplete` after overflow) and close an owned file."""
        with self.lock:
            self._end(complete)
            if self._owned:
                self._file.close()
            else:
                self._file.flush()
