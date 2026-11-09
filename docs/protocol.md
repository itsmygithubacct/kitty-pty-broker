# Protocol version 2

How observers, resumable replay, identity-bound termination and descendant
cessation are negotiated on the wire without changing the frame format.


Version 2 adds observers and resumable replay. It is negotiated per connection
and is invisible to anything that does not ask for it.

**The frame-format version stays at 1 and must never be bumped.** Both peers
reject any other value on every frame, so raising it would break every deployed
peer in both directions at once — including a plain `status` query. That is not
hypothetical here: a broker outlives its frontend by design, and an update
replaces the client binary while brokers from the previous build keep running.

The session version is negotiated inside the attach payload instead, and is
discriminated structurally rather than by a version field:

| Request | Payload | Meaning |
|---|---|---|
| `ATTACH` | 8 bytes | version 1 attach, exactly as before |
| `ATTACH` | 32 bytes | version 2 attach |
| `OBSERVE` | 32 bytes | version 2 read-only attach |
| `TERMINATE` | 0 bytes | end the session, as before |
| `TERMINATE` | 8 bytes | end the session **only if** its `started_millis` equals this big-endian value |

The broker emits an `ATTACH_REPLY` **if and only if** the request was 32 bytes,
so a version 1 peer can never receive a frame type it does not parse. A broker
that predates version 2 answers a 32-byte request with its existing
`invalid request` error, which a version 2 client reports as a protocol error
rather than silently degrading. `attach ID` with no flags still emits an 8-byte
request, so the shipped path works against any broker.

The reply carries the selected version, the journal epoch, the stream offset of
the first byte that follows, and flags for resumed, complete, and truncated. A
client adopts that offset as a cursor and advances it by the size of every
output payload it then receives.

### Identity-bound terminate

A caller that looks at a session and then kills it - `list`, decide, `kill ID` -
has a race it cannot close from outside: between the look and the kill the
session can be replaced under the same ID (a stable `run --id` pane is respawned
exactly that way), and the unconditional `kill` ends the replacement. Reproduced
on the previous build: start `swap`, note its `started_millis`, kill it, start
`swap` again, `kill swap` - the new session dies. A second status query before
the kill only narrows the window.

So the request carries the identity. `TERMINATE` with an 8-byte payload - the
`started_millis` from the status the caller saw - is honoured only if it equals
the broker's own; otherwise the broker answers `identity mismatch` and does
nothing. Each broker process owns exactly one session, so the comparison and the
decision to act are one step inside it, with nothing in between to replace. The
empty `TERMINATE` keeps today's behaviour exactly, and the frame version stays 1
(the two forms differ by payload size alone, like `ATTACH` v1 and v2).

A broker from a build that predates this answers the 8-byte form with its
existing `invalid request` and does nothing - verified against `8cf3eb3` and the
pre-protocol-2 build (`make compatibility`). The library maps the two answers to
distinct results, `KPB_ERR_MISMATCH` and `KPB_ERR_UNSUPPORTED`, and **never**
retries unconditionally: whether to take the risk of a plain kill against a
broker that cannot check is the caller's decision, not the library's.

On the CLI, `kill ID --expect-started MILLIS` exits 3 on a mismatch and 5 when
the broker cannot check; plain `kill ID` is unchanged. The TUI binds its `x`/`y`
confirmation the same way, to the session that was on screen when `x` was
pressed.

### Descendant cessation

`drain ID --expect-started MILLIS` and `kpb_drain_expect` provide a stronger,
opt-in result for transferring ownership of a conversation. Exit 0 means the
broker has reaped every child in its descendant domain. This includes children
created after inspection, detached sessions and double-forked descendants.
Ordinary `kill` still acknowledges the request before termination finishes.

On Linux, a new broker establishes a subreaper boundary before spawning its
command. Drain requires that boundary and pidfd support. It signals direct
children through pidfds; when their parents exit, descendants are adopted into
the same boundary. Only `waitpid(-1, WNOHANG)` reporting `ECHILD`, after the
original command was reaped, permits an acknowledgment. A snapshot of process
IDs or disappearance of the original process group is insufficient.

The same boundary means a live session adopts descendants whose parents exit
first. The broker reaps each one as it exits, while the session runs, so none
is left a zombie; living descendants stay in the domain until they exit or are
drained.

The appended wire frame `DRAIN` (16) carries the existing 8-byte start identity.
Old brokers refuse it without terminating anything; there is no legacy fallback.
Identity mismatch exits 3, unsupported operation exits 5, and a request that was
never sent exits 6. A timeout or transport failure is an unknown outcome, never
permission to start another writer. The C API defaults to a 3500ms caller bound;
an explicit shorter timeout remains authoritative.

The validated server operation gives TERM a 1500ms grace period and bounds its
initial cleanup phase to 2500ms. If cessation cannot be established, it reports
failure and retains its descendant ownership while continuing cleanup. Caller
timeout or disconnection does not release that ownership. Killing the broker
itself can defeat containment, so an EOF without the cessation acknowledgment
must remain unqualified.

### Resume

Within one epoch the journal is strictly append-only, so a resume offset only
becomes invalid when the epoch rolls over — which is exactly what eviction is
here. A request is honoured when the epoch matches and the offset is within the
journal; anything else silently becomes a full replay, reported by the absence
of the resumed flag. Resume never widens what a peer can see: it can only ask
for less than a full replay.

### Observers

- Read-only, enforced by the broker rather than by the client's restraint. Any
  frame an observer sends ends its connection: `DETACH` closes silently,
  `INPUT` and `RESIZE` are refused with an error first. Only the frame header
  is ever read, so an observer cannot make the broker read an attacker-chosen
  payload size.
- Capped at `KPB_OBSERVER_MAX` (8). The next request is refused with
  `KPB_ERR_BUSY` and disturbs neither the accepted set nor the pane.
- **Non-blocking, with a bounded queue.** The read-write client is written
  through a bounded queue too: past a high-water mark the broker stops reading
  the PTY, the kernel's buffer fills, and the shell blocks in `write()` — so a
  frontend that stops reading still stops the shell, but can no longer stop
  the broker, and `status` and `kill` keep working throughout. An observer
  must not have even that much power over the pane, so one that falls behind
  is disconnected rather than buffered. Resume is what makes that cheap — a
  dropped observer reattaches and asks for the bytes it missed.
- Replay on attach is bounded to the newest `KPB_OBSERVER_REPLAY_MAX` (1 MiB),
  prefixed with a terminal reset and flagged as truncated. That bound is kept
  strictly below the queue limit so a fresh replay can never by itself trip the
  drop policy.
- An observer never reaches the PTY: not `apply_size` at admission, not
  `queue_input` afterwards.
- `attached` in the status reply still means the read-write slot is taken.
  Observers deliberately do not set it, because callers filter reusable panes on
  that flag. Observer count is not exposed.

See `CHANGELOG.md` for what changed in the library, including the one behaviour change a caller
can notice: a `kill` that times out while still connecting now reports `KPB_ERR_NOT_SENT`
(exit 6) instead of `KPB_ERR_TIMEOUT`.
