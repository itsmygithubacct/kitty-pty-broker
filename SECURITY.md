# Why an observer cannot escalate

Status: written 2026-07-29 against protocol v2 on `feat/protocol-v2-observers`

Protocol v2 lets processes attach to a pane read-only, alongside the single
read-write frontend. That is a new class of peer with access to a live
terminal, so it deserves an argument rather than an assurance.

The claim being defended: **an observer can see what the pane displays, and
can do nothing else.** Everything below names the mechanism, so a reader can
check it instead of believing it.

## What an observer is assumed to be

A process running as the same user as the broker. `SO_PEERCRED` is checked at
accept (`peer_is_owner`, `src/kitty_pty_broker.c`) and a peer with a different
uid is closed before it can send anything, so this is not a boundary between
users. It is a boundary between **roles**: something that should watch a pane
versus something that should drive it.

That distinction matters because the multiplexer attaches as an observer. A
bug there should not become control of the shell.

## The claims

### 1. An observer cannot write to the PTY

There is one path from a socket to the pane's input: `queue_input()`, reached
only from `handle_client_frame()`, which is called only for
`server->client_fd`. The observe path never calls it and observers are never
stored in `client_fd`.

An observer that sends `KPB_FRAME_INPUT` is not ignored — it is **refused and
disconnected** (`observer_refuse`, `KPB_ERROR_READ_ONLY`). Silence would leave
a caller unsure whether it had worked.

### 2. An observer cannot resize the pane

`apply_size()` reaches `TIOCSWINSZ`, and a resize is visible to the running
program: it delivers `SIGWINCH`, changes what the program draws, and so is a
write in every sense that matters.

The v1 attach path calls `apply_size()` **unconditionally** after admitting a
client, which makes this the easiest mistake in the whole change. The v2
observe path therefore does not call it at all, and the client library sends
zero dimensions for an observer so that a future server reading them could not
be misled either. A `KPB_FRAME_RESIZE` from an observer is refused like input.

### 3. An observer cannot take the read-write slot

`client_fd` is assigned in exactly two places, both on the control path. The
observe path allocates from a separate fixed array and touches `client_fd`
never. `status.attached` is still derived from `client_fd` alone, so an
observer does not even make the pane *look* attached — which matters because
Kilix filters reusable panes on that flag.

### 4. An observer cannot make the broker read what it chooses

`handle_observer_frame()` reads **only the 12-byte header** and never the
payload. Every frame type an observer can send ends the connection, so there
is nothing to desynchronise: a hostile observer cannot declare a large
`payload_size` and make the broker wait for bytes that will not arrive, nor
allocate against a size it invented.

### 5. An observer cannot stall the pane

The read-write client's writes are blocking, and that is correct for it: a
frontend that stops reading should stop the shell, which is backpressure
working as intended. An observer must not have that power over someone else's
session.

So observers get non-blocking sockets and a bounded queue, and one that falls
behind is **disconnected rather than buffered**. Resume-from-offset is what
makes that acceptable: a dropped observer reattaches and asks for the bytes it
missed.

Asserted by test: eight observers attached and never read while a pane floods
a megabyte, and the read-write client still receives every byte.

**Scope, and a correction.** That covers an observer that has finished
connecting. It did not cover the step before, and an earlier version of this
document implied it did. `handle_new_connection()` reads a peer's first frame
inside the event loop, and until 2026-07-29 it read it with no time limit at
all — so a peer that connected and then said nothing stopped the broker
outright: no pane output, no client, no shutdown, until that peer went away.
Reachable by any process running as the same user, which is the trust boundary
this whole document sits inside, but "an observer cannot stall the pane" was
still the wrong thing to have written. The bug predates the observer work; the
v1 accept path has the same shape.

That read now carries an absolute 500 ms budget, shared across the whole frame
so that dribbling a byte at a time cannot extend it. What that buys is a
bound, not immunity: a same-uid peer can still cost the loop half a second per
connection and can reconnect. Removing the cost entirely means driving the
handshake from the poll loop the way `kilix-multiplexer` does, which is a
larger change than this branch should carry.

**And a correction to the correction.** The paragraph above was written when
only `handle_new_connection()` had been bounded, and it was wrong in the same
way the original claim was: `handle_client_frame()` — the next read from the
same peer, once it has attached, in the same event loop — was still calling the
unbounded `receive_frame()`. So a client that completed the handshake and then
sent one byte of a header stopped the broker exactly as completely, one frame
later. Measured: with only the accept path bounded, a status query never
returned; with the client read bounded it returns at the deadline.

Both reads are now bounded — 500 ms in the accept path, 2 s for an attached
client, the longer budget because `send_frame()` writes the header and the
payload as two separate writes, so a legitimate client descheduled between them
is normal rather than hostile. `test_a_stalled_client_does_not_stop_the_broker`
asserts it, and dies on an alarm against the previous build rather than hanging.

The lesson worth recording is not the bug. It is that the first fix was
verified against the case that prompted it and not against the shape of it, and
the document was updated to match the fix rather than the property.

### 6. An observer cannot see more than the frontend

An observer receives the replay journal and subsequent output — the same bytes
the read-write client gets. It does not receive input, so it cannot see what
was typed except where the pane echoes it, which is exactly what a person
looking at the screen would see.

This is the one claim with a caveat worth stating: **a pane's output includes
whatever the pane prints**, so an observer sees passwords that a program
echoes. It does not see input to a program that suppresses echo. That is the
same exposure as the transcript feature, and is a property of watching a
terminal rather than of this mechanism.

### 7. An observer cannot outlive its usefulness

Observers are bounded at `KPB_OBSERVER_MAX` (8); the next is refused. Slots
are reclaimed on disconnect, including a peer that vanishes without a `DETACH`
— asserted by a test that closes 64 connections abruptly and checks the
broker's descriptor count is unchanged.

### 8. An observer cannot make the broker fail differently

Every observer path either succeeds or closes that one connection. The
`server_state` shutdown paths free observer descriptors and queues, including
the `goto fail` routes, and slots are initialised to `-1` before the first
one — otherwise `memset` would leave them at 0 and cleanup would close
descriptor 0.

## What this argument does not cover

- **A compromised broker.** If the broker itself is subverted, none of this
  holds. This is a boundary inside one trust domain, not a sandbox.
- **Resource exhaustion by a same-uid process.** A local process can occupy
  all eight observer slots. It could also fork bombs; same-uid processes are
  not defended against here.
- **The multiplexer's own network exposure.** That is a separate document:
  `kilix-multiplexer/SECURITY.md`.

## How to check this

```sh
make test        # includes every claim above that is assertable
make sanitize    # ASan + UBSan, with reports routed to files
tests/mixed_version.sh
```
