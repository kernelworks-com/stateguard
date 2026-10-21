"""Instance-scoped hooks for the static backend at the two documented pins."""
from contextvars import ContextVar
from functools import wraps


# Synthetic descriptors describe equality of observed keys within this capture,
# not semantic equivalence across models, processes, or captures.
DESCRIPTOR = ('capture', 'opaque', 'lmcache-static', 'kv', 'key', '0', '1', 'opaque', 'opaque', 'opaque')


class _Progress(set):
    def __init__(self, original, capture):
        super().__init__(original)
        self.capture = capture

    def discard(self, key):
        with self.capture.writer.lock:
            super().discard(key)
            self.capture.discard(key)


class Capture:
    def __init__(self, backend, writer, page_address, failed_post_is_quiescent=False):
        self.backend = backend
        self.writer = writer
        self.page_address = page_address
        self.failed_post_is_quiescent = failed_post_is_quiescent
        self.undo = []
        self.pages = {}
        self.slots = {}
        self.objects = {}
        self.keys = {}
        self.sequence = 0
        self.current = ContextVar('stateguard-put', default=None)
        self.active = []

    def unique(self, prefix):
        self.sequence += 1
        return prefix + str(self.sequence)

    def patch(self, obj, name, replacement):
        previous = obj.__dict__.get(name)
        owned = name in obj.__dict__
        setattr(obj, name, replacement)
        self.undo.append((obj, name, previous, owned))

    def wrap(self, obj, name, factory):
        self.patch(obj, name, factory(getattr(obj, name)))

    def track(self, obj):
        if id(obj) in self.objects:
            if obj.get_ref_count() <= 0:
                raise ValueError('source is already reclaimed')
            return self.objects[id(obj)][1]
        address = self.page_address(obj)
        old = self.pages.get(address)
        if old is not None and old['live']:
            raise ValueError('overlapping live MemoryObj aliases are unsupported')
        ref = (old['ref'][0], old['ref'][1] + 1) if old else (self.unique('page-'), 1)
        page = {'ref': ref, 'live': True}
        self.pages[address] = page
        self.objects[id(obj)] = (obj, ref)  # keep object identity stable for this session
        self.writer.allocate(*ref, obj.get_physical_size(), 'source')
        if obj.get_ref_count() <= 0 or obj.meta.pin_count:
            raise ValueError('capture requires live unpinned source objects')

        def ref_method(original):
            @wraps(original)
            def call(*args, **kwargs):
                with self.writer.lock:
                    result = original(*args, **kwargs)
                    count = obj.get_ref_count()
                    if count == 0 and page['live']:
                        self.writer.lifecycle('retire', ref)
                        self.writer.lifecycle('reclaim', ref)
                        page['live'] = False
                    elif count > 0 and not page['live']:
                        raise ValueError('resurrected MemoryObj is unsupported; use a new object')
                    return result
            return call
        def unsupported_pin(*args, **kwargs):
            raise ValueError('pinning during capture is unsupported')
        for name in ('pin', 'unpin'):
            if hasattr(obj, name):
                self.patch(obj, name, unsupported_pin)
        self.wrap(obj, 'ref_count_up', ref_method)
        self.wrap(obj, 'ref_count_down', ref_method)
        return ref

    def discard(self, key):
        entry = self.keys.get(key)
        if entry is not None and not entry['published'] and key in self.backend.key_dict:
            self.writer.emit('publish', entry['state'], *map(str, entry['ref']), entry['op'])
            entry['published'] = True
        for call in self.active:
            if key in call['remaining']:
                call['remaining'].remove(key)
                if not call['remaining']:
                    self.writer.complete(call['cleanup'], call['failed'])
                    self.writer.lifecycle('retire', call['marker'])
                    self.writer.lifecycle('reclaim', call['marker'])
        self.active[:] = [call for call in self.active if call['remaining']]

    def install(self):
        backend = self.backend
        if isinstance(backend.progress_set, _Progress):
            raise ValueError('backend is already instrumented')
        if backend.progress_set:
            raise ValueError('install before any puts are in flight')
        if backend.key_dict:
            raise ValueError('install with an empty key dictionary')
        self.patch(backend, 'progress_set', _Progress(backend.progress_set, self))

        def submit(original):
            @wraps(original)
            def call(keys, memory_objs, *args, **kwargs):
                with self.writer.lock:
                    for obj in memory_objs:
                        self.track(obj)
                return original(keys, memory_objs, *args, **kwargs)
            return call
        self.wrap(backend, 'batched_submit_put_task', submit)

        def mem_to_storage(original):
            def schedule(keys, mem_objs):
                if not keys:
                    return original(keys, mem_objs)
                # This factory executes before run_coroutine_threadsafe returns.
                with self.writer.lock:
                    call = {'hold': self.unique('put-'), 'remaining': set(keys), 'failed': False,
                            'sources': [self.track(obj) for obj in mem_objs], 'slots': [],
                            'cleanup': self.unique('cleanup-'), 'marker': (self.unique('metadata-'), 1)}
                    self.writer.submit(call['hold'], [('r', ref) for ref in call['sources']])
                    self.writer.allocate(*call['marker'], 0, 'put-metadata')
                    self.writer.submit(call['cleanup'], [('w', call['marker'])])
                    self.active.append(call)
                async def run():
                    token = self.current.set(call)
                    try:
                        return await original(keys, mem_objs)
                    finally:
                        self.current.reset(token)
                return run()
            return schedule
        self.wrap(backend, 'mem_to_storage', mem_to_storage)

        def pop(original):
            def call(*args, **kwargs):
                with self.writer.lock:
                    index = original(*args, **kwargs)
                    prior = self.slots.get(index)
                    if prior is not None and prior['live']:
                        raise ValueError('pool returned a live slot')
                    ref = (prior['ref'][0], prior['ref'][1] + 1) if prior else (self.unique('slot-'), 1)
                    self.writer.allocate(*ref, 0, 'storage')  # byte extent unavailable at pop
                    self.slots[index] = {'ref': ref, 'live': True}
                    return index
            return call
        self.wrap(backend.pool, 'pop', pop)

        def push(original):
            def call(index):
                with self.writer.lock:
                    slot = self.slots[index]
                    self.writer.lifecycle('retire', slot['ref'])
                    self.writer.lifecycle('reclaim', slot['ref'])
                    slot['live'] = False
                    return original(index)
            return call
        self.wrap(backend.pool, 'push', push)

        def add(original):
            def call(key, meta, index):
                with self.writer.lock:
                    result = original(key, meta, index)
                    context = self.current.get()
                    state = self.unique('state-')
                    ref = self.slots[index]['ref']
                    entry = {'state': state, 'ref': ref, 'op': '-', 'published': False}
                    self.keys[key] = entry
                    self.writer.emit('bind_state', *map(str, ref), state, *DESCRIPTOR)
                    context['slots'].append(entry)
                    return result
            return call
        self.wrap(backend, 'add_key_to_dict', add)

        def post(original):
            def call(handle):
                context = self.current.get()
                if context is None:
                    raise ValueError('only static puts are supported during capture')
                with self.writer.lock:
                    operation = self.unique('transfer-')
                    for entry in context['slots']:
                        entry['op'] = operation
                    self.writer.submit(operation, [('r', ref) for ref in context['sources']] +
                                       [('w', entry['ref']) for entry in context['slots']])
                # Never hold the capture lock across blocking transport work.
                try:
                    result = original(handle)
                except Exception:
                    with self.writer.lock:
                        context['failed'] = True
                        if self.failed_post_is_quiescent:
                            self.writer.complete(operation, True)
                            self.writer.complete(context['hold'], True)
                        else:
                            self.writer.emit('coverage_gap', 'unproven-failed-transfer-completion')
                    raise
                with self.writer.lock:
                    self.writer.complete(operation)
                    self.writer.complete(context['hold'])
                return result
            return call
        self.wrap(backend.agent, 'post_blocking', post)

        def collect(original):
            def call(keys):
                result = original(keys)
                with self.writer.lock:
                    for key, metadata in zip(keys, result):
                        if metadata is not None:
                            entry = self.keys[key]
                            self.writer.emit('consume', self.unique('read-'), entry['state'],
                                             *map(str, entry['ref']), *DESCRIPTOR)
                return result
            return call
        self.wrap(backend, '_collect_metadata_with_lock', collect)
        return self

    def close(self):
        # Caller must drain the event loop and stop all producers first.
        for obj, name, previous, owned in reversed(self.undo):
            if obj is self.backend and name == 'progress_set':
                previous.clear()
                previous.update(obj.progress_set)
            if owned:
                setattr(obj, name, previous)
            else:
                delattr(obj, name)
        self.undo.clear()
        self.objects.clear()
        self.pages.clear()
        self.slots.clear()
        self.keys.clear()
        self.active.clear()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def instrument(backend, writer, *, page_address=None, failed_post_is_quiescent=False):
    """Instrument one empty static backend until the returned context closes.

    page_address must identify physical pages consistently. The default uses
    the backing tensor's data pointer; addresses never appear in emitted IDs.
    """
    if page_address is None:
        page_address = lambda obj: obj.raw_data.data_ptr()
    capture = Capture(backend, writer, page_address, failed_post_is_quiescent)
    try:
        return capture.install()
    except BaseException:
        capture.close()
        raise
