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

## Library contract

The public API is in `include/kitty_pty_broker.h`. It supports:

- cryptographically random or caller-supplied stable session IDs;
- spawning a command under an independently owned PTY;
- attach, input, resize, output, detach, status, list, and terminate operations;
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
