# kitty-pty-broker

`kitty-pty-broker` is a small C11/POSIX library and companion executable that
separates a terminal pane's lifetime from its graphical frontend without
placing a terminal multiplexer in the byte stream.

An independent broker process owns the real PTY and shell process group. A
frontend attaches over a private Unix socket through the included client; the
client forwards terminal bytes unchanged and propagates `SIGWINCH` dimensions.
If the frontend exits or crashes, only that attachment disappears. The PTY,
shell, and applications continue running and can be attached again.

Because the broker does not parse or wrap terminal output, live Kitty graphics
protocol commands—including local file and POSIX shared-memory transfers—retain
their native behavior. This differs from tmux-style persistence, which inserts
another terminal parser and requires graphics passthrough.

## Build and test

```sh
make
make test
make sanitize
```

The build produces:

- `libkitty-pty-broker.so` and `libkitty-pty-broker.a`
- `kitty-pty-broker`, a CLI built on the public library

The only non-libc dependency is the platform PTY utility library (`libutil` on
Linux).

## CLI

```sh
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" run --id work -- bash
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" \
    run --id work --transcript ~/.local/state/work.log -- bash
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" attach work
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" attach work --resume 0:4096
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" observe work
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" observe work --from 0:4096
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" list --json
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" status work --json
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" kill work
kitty-pty-broker --runtime-dir "$XDG_RUNTIME_DIR/kpb" tui
```

`run` starts the broker and attaches the current terminal. Disconnecting that
client leaves the session alive. `kill` is the deliberate termination path;
the broker first sends `SIGTERM` to every process group in the pane's terminal
session, then `SIGKILL` after a bounded grace period.

`tui` opens the interactive session manager. Detached sessions are sorted
first; use the arrow keys or `j`/`k` to select one, Enter to attach, `x` to
request termination (with confirmation), `r` to refresh, and `q` to quit. The
interface uses only ANSI terminal controls and adds no runtime dependency.

`observe` attaches read-only. It renders the pane but forwards nothing: keys
are consumed locally and `Ctrl-]` leaves. `SIGWINCH` is ignored, so watching a
pane cannot resize it. Any number of observers may watch a pane that is already
attached, or one that is not attached at all.

Both `attach` and `observe` print `cursor=EPOCH:OFFSET` to stderr on exit. Pass
that back as `--resume` or `--from` to continue where you stopped instead of
repainting from the start of the journal.

## Protocol version 2

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
- **Non-blocking, with a bounded queue.** The read-write client keeps its
  blocking write, which is correct for it: a frontend that stops reading should
  stop the shell. An observer must not have that power, so one that falls
  behind is disconnected rather than buffered. Resume is what makes that cheap —
  a dropped observer reattaches and asks for the bytes it missed.
- Replay on attach is bounded to the newest `KPB_OBSERVER_REPLAY_MAX` (1 MiB),
  prefixed with a terminal reset and flagged as truncated. That bound is kept
  strictly below the queue limit so a fresh replay can never by itself trip the
  drop policy.
- An observer never reaches the PTY: not `apply_size` at admission, not
  `queue_input` afterwards.
- `attached` in the status reply still means the read-write slot is taken.
  Observers deliberately do not set it, because callers filter reusable panes on
  that flag. Observer count is not exposed.

## Library contract

The public API is in `include/kitty_pty_broker.h`. It supports:

- cryptographically random or caller-supplied stable session IDs;
- spawning a command under an independently owned PTY;
- attach, input, resize, output, detach, status, list, and terminate operations;
- read-only observation and resumable replay through `kpb_observe` and
  `kpb_attach_with_options`; the original `kpb_attach` is unchanged and remains
  the version 1 entry point;
- bounded input queuing so large pastes respect PTY backpressure without loss;
- versioned framed communication over owner-only Unix sockets;
- a bounded replay journal for reconstructing a newly attached terminal;
- an optional durable transcript of session output.

Runtime and session directories must be absolute, real directories owned by
the current user. They are forced to mode `0700`; sockets and metadata are
private. Session IDs are conservative single path components, and connecting
peers are checked with `SO_PEERCRED` on Linux.

## Replay and graphics

Live bytes are always transparent. The broker also journals output so a fresh
frontend can reconstruct terminal state. The default journal limit is 64 MiB.
When the limit is reached, the broker starts a new replay epoch with a terminal
reset and marks the status as incomplete; this bounds storage without
pretending an arbitrarily long raw terminal stream is a serializable snapshot.

## Transcripts

`--transcript PATH` additionally records session output to a durable log. This
is a different guarantee from the replay journal, and the two are deliberately
not shared: the journal exists to repaint one reattaching client and therefore
discards all history when it overflows, while a transcript keeps the newest
bytes and never emits a terminal reset.

- `--transcript-limit BYTES` bounds the file (default 8 MiB). On overflow the
  newest three quarters of the budget are slid to the front of the same inode
  and the rest is dropped, so a single long-lived writer keeps its descriptor.
- `--transcript-graphics elide|keep` selects payload handling. The default
  `elide` replaces the body of kitty graphics APC sequences (`ESC _ G … ESC \`)
  with a short byte-count marker, because one pane running a pixel desktop, a
  browser, or `icat` can emit megabytes per second and would otherwise evict
  every readable line within seconds. `keep` records the stream verbatim.
- The transcript is written by the broker itself, so an unattached or detached
  pane is still captured. Only PTY *output* is recorded; input reaches the file
  solely through terminal echo, so a password prompt that suppresses echo is
  not captured.
- The file is created `0600` and opened `O_NOFOLLOW`. An existing transcript is
  continued, not truncated, so a recovered pane keeps its history. Transcript
  failures are non-fatal: the broker closes the log and the pane keeps running.

Elision makes a default transcript a faithful record of *text*, not a byte-exact
capture of the stream. Use `keep` when the graphics bytes themselves are the
subject of the investigation.

Local file or shared-memory graphics resources can be consumed and removed by
the first frontend, so their historical escape sequences are not independently
replayable. Long-running graphical applications receive a resize after attach
and can redraw. Hosts that require exact static-image restoration should use
direct (`t=d`) Kitty graphics transmission or provide terminal-state
checkpoints through a future protocol extension.

## Scope

This library protects panes from frontend loss. It cannot survive the broker
itself being sent `SIGKILL`, the OOM killer, a reboot, or loss of the user
session. Threads are deliberately not used as a lifetime boundary because all
threads die with their process.

## License

MIT
