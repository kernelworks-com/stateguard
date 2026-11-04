# Trace format, schema 1

A `.sgtrace` file records the lifecycle events of cache memory in the order the
producer observed them. This document is enough to write a producer in any
language. The reference implementation is `serialize_trace` and `parse_trace`
in [`src/trace.cpp`](../src/trace.cpp); the event types are in
[`include/stateguard/types.hpp`](../include/stateguard/types.hpp). What the
checker concludes from the events is in the
[technical reference](REFERENCE.md).

The format is a readable interchange format for fixtures and captures. It is
versioned by its schema major number, not an ABI promise.

## Lines and fields

- A trace is UTF-8 text made of lines separated by `\n`. A trailing `\r` on a
  line is removed before parsing. Empty lines are skipped. Write `\n` and end
  the last line with `\n`.
- Each line is a list of fields separated by `|`. The first field of an event
  line is its kind.
- There are no comments, quoting, or escapes. A value that contains a reserved
  character cannot be written.
- Integers are unsigned decimal ASCII digits (`0`–`9`) in the range 0 to
  2^64 − 1. There is no sign, no whitespace, and no other notation. Write them
  without leading zeros; the serializer never emits them.

### Header

The first nonempty line is the header:

```text
STATEGUARD_TRACE|1
```

The second field is the schema major version. A reader accepts only the major
it implements, which is 1 here, and rejects any other value. A file without a
header is rejected.

### Value types

| Type | Rule |
|---|---|
| identifier | nonempty; must not contain `\|`, `;`, `:`, `,`, `\n`, or `\r` |
| text | nonempty; must not contain `\|`, `\n`, or `\r` (`;`, `:`, and `,` are allowed) |
| uint | integer as above |
| generation | uint, nonzero |
| epoch | uint, nonzero |

Identifiers name allocations, states, operations, requests, endpoints, route
tags, domains, clocks, and reasons. Text is used only in descriptor fields
(other than the two token bounds, which are uints).

The reader is more permissive than this table in places: it accepts `;`, `:`,
and `,` in some identifier positions, an empty `allocate` domain, and a zero
generation or `connect` epoch. Producers must not rely on that. The serializer
rejects all of them, and the checker reports an identifier with a reserved
character or a zero generation as `invalid_state_transition`.

### Compound values

An **allocation reference** is two fields: `<allocation>|<generation>`.

A **semantic descriptor** is ten fields, in this order:

| # | Field | Type |
|---|---|---|
| 1 | namespace | text |
| 2 | model revision | text |
| 3 | adapter | text |
| 4 | component | text |
| 5 | prefix | text |
| 6 | token begin | uint |
| 7 | token end | uint, not less than token begin |
| 8 | representation | text |
| 9 | layout | text |
| 10 | position metadata | text |

Every descriptor field is nonempty. Two descriptors are compatible only if all
ten fields are equal.

A **lease list** is zero or more leases joined by `;`. A lease is
`<access>:<allocation>:<generation>`, where access is `r` (read) or `w`
(write). An empty field means no leases.

A **dependency list** is zero or more operation identifiers joined by `,`. An
empty field means no dependencies.

A **route** is `<endpoint>:<epoch>:<tag>`: a transport endpoint identifier, the
endpoint's connection epoch, and the backend's identifier for the transfer.

## Events

Fields are listed in order after the kind. `[...]` marks an optional trailing
field; every other field is required, and a line with any other field count is
rejected.

| Kind | Fields |
|---|---|
| `allocate` | allocation, generation, bytes (uint), domain |
| `bind_state` | allocation, generation, state, descriptor (10 fields) |
| `submit` | operation, request, lease list, dependency list, [route] |
| `publish` | state, allocation, generation, completed operation or `-` |
| `consume` | request, state, allocation, generation, expected descriptor (10 fields) |
| `cancel` | request |
| `cancel_signal` | request |
| `finish` | request |
| `complete` | operation, [outcome], [route evidence] |
| `connect` | endpoint, epoch |
| `expire` | operation, reason |
| `retire` | allocation, generation |
| `reclaim` | allocation, generation |
| `coverage_gap` | reason |
| `time` | clock, nanoseconds (uint) |
| `end` | `complete` or `incomplete` |

So `bind_state` has 14 fields and `consume` has 15, counting the kind.

What each event means:

- `allocate`: the allocation's memory becomes live at this generation. A reuse
  of the same allocation must use a larger generation. `domain` names where the
  memory lives, for example `cpu`.
- `bind_state`: the allocation generation now holds the named logical state,
  described by the descriptor.
- `submit`: an asynchronous operation started for a request. Each lease names
  an allocation generation the operation may read or write until it completes.
  Dependencies name operations it waits for. The optional route ties the
  operation to one backend transfer; two operations cannot share a route.
- `publish`: the state becomes visible to consumers. The fourth field names the
  completed operation that wrote it, or `-` if no proof is given.
- `consume`: a request reads the state from the allocation generation and
  expects the descriptor.
- `cancel`: the scheduler acknowledged the request's cancellation and released
  its consumer role. It does not end the request's operations.
- `cancel_signal`: the process received a cancellation signal for the request.
  The scheduler may still be using it.
- `finish`: the request ended normally and its consumer role is released.
- `complete`: the operation's access ended, so it no longer touches its leased
  memory. The outcome is `succeeded` or `failed`.
- `connect`: the endpoint started a connection epoch, ending the previous one.
  Epochs of an endpoint must increase.
- `expire`: the host stopped waiting for the operation (a timeout, an expired
  lease, a disconnected peer). Its leases stay held.
- `retire`: the allocation generation takes no new users.
- `reclaim`: the allocation generation was returned to its pool and may be
  reused.
- `coverage_gap`: events may be missing at this point, for example
  `capture-overflow`.
- `time`: the following events happened at this reading, in nanoseconds, of
  the named monotonic clock, until the next `time` line.
- `end`: the capture ended. `complete` says no event was lost; `incomplete`
  says some may have been. The reader also accepts `1`/`true` and `0`/`false`,
  but producers write `complete` or `incomplete`. A trace has at most one
  `end`, as its last event; the checker reports any event after it.

### `complete` forms

| Line | Meaning |
|---|---|
| `complete\|op` | succeeded, no evidence |
| `complete\|op\|succeeded` | succeeded, no evidence (accepted; the serializer writes the form above) |
| `complete\|op\|failed` | failed, no evidence |
| `complete\|op\|succeeded\|peer-a:1:7` | succeeded; the backend's completion named route `peer-a:1:7` |
| `complete\|op\|failed\|peer-a:1:7` | failed, with evidence |

Evidence is only valid after an explicit outcome. It must be the route the
backend's completion notification named, copied from that notification, not
from the producer's own submission record.

## Examples

A clean shared-prefix lifecycle:

```text
STATEGUARD_TRACE|1
allocate|buffer-1|1|4096|cpu
bind_state|buffer-1|1|state-1|demo|model-r1|cpu-cache|layer-0|prefix-42|0|16|fp16|row-major|absolute
submit|copy-1|request-1|w:buffer-1:1|
complete|copy-1
publish|state-1|buffer-1|1|copy-1
consume|request-1|state-1|buffer-1|1|demo|model-r1|cpu-cache|layer-0|prefix-42|0|16|fp16|row-major|absolute
consume|request-2|state-1|buffer-1|1|demo|model-r1|cpu-cache|layer-0|prefix-42|0|16|fp16|row-major|absolute
cancel|request-1
finish|request-2
retire|buffer-1|1
reclaim|buffer-1|1
end|complete
```

A routed transfer that survives a reconnect, with timestamps and a cancelled
request:

```text
STATEGUARD_TRACE|1
time|steady|1000
connect|peer-a|1
allocate|staging-1|1|4096|cpu
allocate|cache-1|1|4096|cpu
bind_state|cache-1|1|state-1|demo|model-r1|cpu-cache|layer-0|prefix-42|0|16|fp16|row-major|absolute
submit|load-1|loader|r:staging-1:1;w:cache-1:1||peer-a:1:7
time|steady|2000
expire|load-1|timeout
connect|peer-a|2
complete|load-1|succeeded|peer-a:1:7
publish|state-1|cache-1|1|load-1
retire|staging-1|1
reclaim|staging-1|1
consume|request-1|state-1|cache-1|1|demo|model-r1|cpu-cache|layer-0|prefix-42|0|16|fp16|row-major|absolute
time|steady|3000
cancel_signal|request-1
cancel|request-1
retire|cache-1|1
reclaim|cache-1|1
end|complete
```

A capture that overflowed and a reclaim that happened too early. The checker
reports the reclaim as an unconfirmed finding, because the missing events may
have contained the completion:

```text
STATEGUARD_TRACE|1
allocate|buffer-1|1|4096|cpu
submit|copy-1|request-1|r:buffer-1:1|
coverage_gap|capture-overflow
retire|buffer-1|1
reclaim|buffer-1|1
end|incomplete
```

Every block above that starts with the header is checked by the test suite
(`tests/check_trace_format_doc.py`): `stateguard-check` must parse it.

## Which tools read which events

| Event | Checker | Timeline | Reducer |
|---|---|---|---|
| `allocate`, `retire`, `reclaim`, `submit`, `complete`, `consume`, `cancel`, `finish`, `coverage_gap`, `end` | used | used | kept or removed as a whole event |
| `bind_state`, `publish` | used | ignored | kept or removed |
| `connect`, `expire` | used | ignored | kept or removed |
| `cancel_signal` | identity validated only; releases nothing | signal receipt | kept or removed |
| `time` | clock identity validated only; checker uses trace order | timestamps | removed, since they never change a finding |

The checker orders events by their position in the file, never by timestamps.
The timeline requires one clock whose readings do not decrease. The reducer
only deletes events; it never edits, renames, or reorders them.

## Producer contract

**Order.** Record each event before its effect can be observed by anything
else. Record `submit` before the operation can touch memory, `publish` before
a consumer can find the state, `reclaim` before the pool can hand the memory out again, and `allocate`
before the next generation is used. Record `complete` only after the access has
ended. Events from several threads must be recorded under the lock (or other
ordering) that makes the effect visible; otherwise the trace order is only the
order in which the recorder was called.

**Completion evidence.** Record `complete` only when the producer has proof
that the operation no longer accesses its leased memory: the backend's
completion callback, a synchronizing wait on the operation, or an equivalent
signal from the code that performs the access. A timeout, an expired lease, a
disconnected peer, or a new connection epoch is not proof; record `expire` or
`connect` instead. If the access failed, record `failed`; reporting a failure
as success hides `failed_write_publication`. If the operation has a route,
record the route from the backend's notification as evidence; a completion
without evidence for a routed operation makes the trace inconclusive.

**Loss.** If events were dropped, record `coverage_gap` at the point of loss
(or as soon as the loss is known) and end with `end|incomplete`. Never end a
lossy capture with `end|complete`. A trace without an `end` line is treated as
truncated.

**Identities.** Use identifiers that stay unique for the life of the trace. A
reused allocation gets a larger generation; the allocation identifier itself
can repeat.

## Compatibility

- Within schema 1, new information is added only as optional trailing fields
  or new event kinds, and a trace that does not use them is byte-for-byte
  unchanged. The route field of `submit` and the outcome and evidence fields
  of `complete` were added this way.
- A reader rejects any line it does not understand: an unknown kind, a field
  count it does not expect, or an invalid value. An older reader therefore
  rejects a trace that uses a newer field instead of misreading it (for
  example, reading a failed completion as a success).
- A change that alters the meaning of an existing line requires a new schema
  major. A reader rejects an unknown major.
- The serializer output is canonical: parsing a trace written by it and
  serializing it again gives the same bytes. Every fixture in
  `tests/fixtures/` is tested for this.

`stateguard-check` exits with 0 (clean), 1 (violation), or 2 (inconclusive)
for any trace it can parse, and with 3 when the file cannot be read or parsed.
