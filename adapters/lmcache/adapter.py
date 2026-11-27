"""Instance-scoped hooks for the static and dynamic put paths at the two documented pins.

Lock order (see docs/LMCACHE.md, "Locking"): backend key_lock, then backend
progress_lock, then the writer lock, then the descriptor-pool lock or a
MemoryObj lock (and the allocator lock it takes). No hook acquires a backend
lock while it holds the writer lock, and no hook holds the writer lock across
a transport call.
"""
from contextvars import ContextVar
from functools import wraps

from .trace import TraceWriter, identifier


# Synthetic descriptors describe equality of observed keys within this capture,
# not semantic equivalence across models, processes, or captures.
DESCRIPTOR = ('capture', 'opaque', 'lmcache-static', 'kv', 'key', 0, 1, 'opaque', 'opaque', 'opaque')
DYNAMIC_DESCRIPTOR = ('capture', 'opaque', 'lmcache-dynamic', 'kv', 'key', 0, 1, 'opaque', 'opaque', 'opaque')
PUT_PATHS = ('static', 'dynamic')


class _Progress(set):
    def __init__(self, original, capture):
        super().__init__(original)
        self.capture = capture

    def discard(self, key):
        # Called by the backend with progress_lock held: progress_lock -> writer lock.
        with self.capture.writer.lock:
            super().discard(key)
            self.capture.discard(key)


def _quiescence(failed_post_is_quiescent, quiescent_transport):
    if type(failed_post_is_quiescent) is not bool:
        raise ValueError('failed_post_is_quiescent must be a bool')
    if not failed_post_is_quiescent:
        if quiescent_transport is not None:
            raise ValueError('quiescent_transport is only meaningful with failed_post_is_quiescent=True')
        return None
    if quiescent_transport is None:
        raise ValueError('failed_post_is_quiescent=True requires quiescent_transport naming the '
                         'transport whose failed transfers are known to be quiescent; see '
                         'docs/LMCACHE.md, "Quiescence contract"')
    if not isinstance(quiescent_transport, str):
        raise ValueError('quiescent_transport must be a str naming the transport')
    return identifier(quiescent_transport, 'quiescent_transport')


class Capture:
    def __init__(self, backend, writer, page_address, failed_post_is_quiescent=False,
                 quiescent_transport=None, put_path='static'):
        if put_path not in PUT_PATHS:
            raise ValueError(f'put_path must be one of {PUT_PATHS}, not {put_path!r}')
        self.quiescent_transport = _quiescence(failed_post_is_quiescent, quiescent_transport)
        self.backend = backend
        self.writer = writer
        self.page_address = page_address
        self.failed_post_is_quiescent = failed_post_is_quiescent
        self.put_path = put_path
        self.undo = []
        self.pages = {}
        self.slots = {}
        self.objects = {}
        self.keys = {}
        self.pending = {}
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

    def context(self, what):
        context = self.current.get()
        if context is None:
            raise ValueError(f'only {self.put_path} puts scheduled through mem_to_storage '
                             f'are supported during capture ({what})')
        return context

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
                # writer lock -> MemoryObj lock -> allocator lock (on free).
                with self.writer.lock:
                    result = original(*args, **kwargs)
                    count = obj.get_ref_count()
                    if count == 0 and page['live']:
                        self.writer.retire(*ref)
                        self.writer.reclaim(*ref)
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

    # Transfer evidence; callers hold the writer lock -------------------------

    def submitted(self, context):
        operation = self.unique('transfer-')
        context['op'] = operation
        for entry in context['slots']:
            entry['op'] = operation
        self.writer.submit(operation, 'put', [('r', ref) for ref in context['sources']] +
                           [('w', entry['ref']) for entry in context['slots']])
        return operation

    def succeeded(self, context):
        self.writer.complete(context['op'])
        self.writer.complete(context['hold'])
        context['transfer'] = 'succeeded'
        for entry in context['slots']:
            entry['written'] = True

    def failed(self, context):
        context['failed'] = True
        if self.failed_post_is_quiescent:
            self.writer.complete(context['op'], 'failed')
            self.writer.complete(context['hold'], 'failed')
            context['transfer'] = 'failed'
        else:
            self.writer.coverage_gap('unproven-failed-transfer-completion')
            context['transfer'] = 'unknown'

    def observe(self, handle, state):
        with self.writer.lock:
            item = self.pending.get(id(handle))
            if item is None or item[0] is not handle or state not in ('DONE', 'ERR'):
                return
            del self.pending[id(handle)]
            if state == 'DONE':
                self.succeeded(item[1])
            else:
                self.failed(item[1])

    def visible(self, key, entry):
        if self.put_path == 'static':
            return key in self.backend.key_dict  # snapshot; key_lock is not taken here
        return entry['written']

    def discard(self, key):
        entry = self.keys.get(key)
        if entry is not None and not entry['published'] and self.visible(key, entry):
            self.writer.publish(entry['state'], *entry['ref'], entry['op'])
            entry['published'] = True
        elif (entry is not None and self.put_path == 'dynamic' and entry['live']
              and not entry['written'] and entry['context']['transfer'] in ('failed', 'none')):
            # Failed dynamic write whose access is known to have ended.
            self.writer.retire(*entry['ref'])
            self.writer.reclaim(*entry['ref'])
            entry['live'] = False
        for call in self.active:
            if key in call['remaining']:
                call['remaining'].remove(key)
                if not call['remaining']:
                    self.writer.complete(call['cleanup'], 'failed' if call['failed'] else 'succeeded')
                    self.writer.retire(*call['marker'])
                    self.writer.reclaim(*call['marker'])
        self.active[:] = [call for call in self.active if call['remaining']]

    # Hooks shared by both put paths -------------------------------------------

    def install_common(self):
        backend = self.backend
        if isinstance(backend.progress_set, _Progress):
            raise ValueError('backend is already instrumented')
        if backend.progress_set:
            raise ValueError('install before any puts are in flight')
        self.patch(backend, 'progress_set', _Progress(backend.progress_set, self))

        def submit(original):
            @wraps(original)
            def call(keys, memory_objs, *args, **kwargs):
                with self.writer.lock:
                    for obj in memory_objs:
                        self.track(obj)
                # The original takes key_lock/progress_lock; never under the writer lock.
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
                            'cleanup': self.unique('cleanup-'), 'marker': (self.unique('metadata-'), 1),
                            'op': None, 'transfer': 'none'}
                    self.writer.submit(call['hold'], 'put', [('r', ref) for ref in call['sources']])
                    self.writer.allocate(*call['marker'], 0, 'put-metadata')
                    self.writer.submit(call['cleanup'], 'put', [('w', call['marker'])])
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

        def post(original):
            def call(handle):
                context = self.context('post_blocking')
                with self.writer.lock:
                    self.submitted(context)
                # Never hold the capture lock across blocking transport work.
                try:
                    result = original(handle)
                except Exception:
                    with self.writer.lock:
                        self.failed(context)
                    raise
                with self.writer.lock:
                    self.succeeded(context)
                return result
            return call
        self.wrap(backend.agent, 'post_blocking', post)

    # Static put path ----------------------------------------------------------

    def install_static(self):
        backend = self.backend
        if backend.key_dict:
            raise ValueError('install with an empty key dictionary')

        def pop(original):
            def call(*args, **kwargs):
                # writer lock -> pool lock; also reached with key_lock held (eviction).
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
                # Reached from remove() with key_lock held during submit-time
                # eviction: key_lock -> progress_lock -> writer lock -> pool lock.
                with self.writer.lock:
                    slot = self.slots[index]
                    self.writer.retire(*slot['ref'])
                    self.writer.reclaim(*slot['ref'])
                    slot['live'] = False
                    return original(index)
            return call
        self.wrap(backend.pool, 'push', push)

        def add(original):
            def call(key, meta, index):
                context = self.context('add_key_to_dict')
                # The original takes key_lock, so it must not run under the
                # writer lock (eviction holds key_lock and then needs the writer
                # lock in push). Binding is recorded first: the key cannot be
                # seen or evicted before the original inserts it.
                with self.writer.lock:
                    previous = self.keys.get(key)
                    state = self.unique('state-')
                    ref = self.slots[index]['ref']
                    entry = {'state': state, 'ref': ref, 'op': None, 'published': False}
                    self.keys[key] = entry
                    self.writer.bind_state(*ref, state, DESCRIPTOR)
                    context['slots'].append(entry)
                try:
                    return original(key, meta, index)
                except BaseException:
                    with self.writer.lock:
                        context['slots'].remove(entry)
                        if previous is None:
                            self.keys.pop(key, None)
                        else:
                            self.keys[key] = previous
                    raise
            return call
        self.wrap(backend, 'add_key_to_dict', add)

        def collect(original):
            def call(keys):
                result = original(keys)
                # Recorded after key_lock is released; an eviction in between is
                # recorded first, which is conservative for the read that follows.
                with self.writer.lock:
                    for key, metadata in zip(keys, result):
                        if metadata is not None:
                            entry = self.keys[key]
                            self.writer.consume(self.unique('read-'), entry['state'],
                                                *entry['ref'], DESCRIPTOR)
                return result
            return call
        self.wrap(backend, '_collect_metadata_with_lock', collect)

    # Dynamic put path ---------------------------------------------------------

    def install_dynamic(self):
        backend = self.backend

        def acquire(original):
            def call(keys, mem_indices, storage_indices, page_size, *args, **kwargs):
                write = kwargs.get('write', args[0] if args else False)
                if not write:
                    return original(keys, mem_indices, storage_indices, page_size, *args, **kwargs)
                context = self.context('_acquire_storage_handle')
                with self.writer.lock:
                    for key in keys:
                        prior = self.keys.get(key)
                        if prior is not None and prior['live']:
                            raise ValueError('re-putting a live dynamic key is unsupported')
                # File/object creation and registration: no writer lock held.
                try:
                    result = original(keys, mem_indices, storage_indices, page_size, *args, **kwargs)
                except Exception:
                    with self.writer.lock:
                        # No transfer handle exists, so nothing was posted.
                        context['failed'] = True
                        self.writer.complete(context['hold'], 'failed')
                    raise
                size = page_size if type(page_size) is int and page_size >= 0 else 0
                with self.writer.lock:
                    for key in keys:
                        prior = self.keys.get(key)
                        ref = (prior['ref'][0], prior['ref'][1] + 1) if prior else (self.unique('object-'), 1)
                        self.writer.allocate(*ref, size, 'storage')
                        state = self.unique('state-')
                        self.writer.bind_state(*ref, state, DYNAMIC_DESCRIPTOR)
                        entry = {'state': state, 'ref': ref, 'op': None, 'published': False,
                                 'written': False, 'live': True, 'context': context}
                        self.keys[key] = entry
                        context['slots'].append(entry)
                return result
            return call
        self.wrap(backend, '_acquire_storage_handle', acquire)

        def post_async(original):
            def call(handle):
                context = self.context('post_async')
                with self.writer.lock:
                    self.submitted(context)
                    self.pending[id(handle)] = (handle, context)
                try:
                    state = original(handle)
                except Exception:
                    with self.writer.lock:
                        self.pending.pop(id(handle), None)
                        self.failed(context)
                    raise
                self.observe(handle, state)
                return state
            return call
        self.wrap(backend.agent, 'post_async', post_async)

        def check_state(original):
            def call(handle, *args, **kwargs):
                state = original(handle, *args, **kwargs)  # transport query: no writer lock
                self.observe(handle, state)
                return state
            return call
        self.wrap(backend.agent.nixl_agent, 'check_xfer_state', check_state)

        def remove(original):
            def call(key, *args, **kwargs):
                # Neither backend lock is taken by the dynamic remove().
                with self.writer.lock:
                    removed = original(key, *args, **kwargs)
                    entry = self.keys.get(key)
                    if removed and entry is not None and entry['live'] and entry['published']:
                        self.writer.retire(*entry['ref'])
                        self.writer.reclaim(*entry['ref'])
                        entry['live'] = False
                    return removed
            return call
        self.wrap(backend, 'remove', remove)

    def install(self):
        self.install_common()
        if self.put_path == 'static':
            self.install_static()
        else:
            self.install_dynamic()
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
        self.pending.clear()
        self.active.clear()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def instrument(backend, writer=None, *, page_address=None, put_path='static', clock=False,
               failed_post_is_quiescent=False, quiescent_transport=None):
    """Instrument one empty backend until the returned context closes.

    put_path is 'static' (NixlStaticStorageBackend) or 'dynamic'
    (NixlDynamicStorageBackend, async or sync mode). writer defaults to a new
    TraceWriter; clock=True stamps it with monotonic time (an explicit writer
    must then have been created with a clock). The returned capture's
    `writer` attribute is the writer in use.

    failed_post_is_quiescent=True records a failed transfer as a failed
    completion instead of a coverage gap. It is refused unless
    quiescent_transport names the transport whose failures are known to stop
    all access (for the CPU mocks: 'mock'); see docs/LMCACHE.md.

    page_address must identify physical pages consistently. The default uses
    the backing tensor's data pointer; addresses never appear in emitted IDs.
    """
    if type(clock) is not bool:
        raise ValueError('clock must be a bool')
    if writer is None:
        writer = TraceWriter(clock=True if clock else None)
    elif clock and writer.clock_name is None:
        raise ValueError('clock=True needs a writer created with a clock, '
                         'for example TraceWriter(clock=True), or writer=None')
    if page_address is None:
        page_address = lambda obj: obj.raw_data.data_ptr()
    capture = Capture(backend, writer, page_address, failed_post_is_quiescent,
                      quiescent_transport, put_path)
    try:
        return capture.install()
    except BaseException:
        capture.close()
        raise
