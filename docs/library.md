# Library contract, replay and transcripts

The public C API in [`include/kitty_pty_broker.h`](../include/kitty_pty_broker.h),
the replay journal, durable transcripts, and what the broker does not protect
against.

## Library contract

The public API is in `include/kitty_pty_broker.h`. It supports:

- cryptographically random or caller-supplied stable session IDs;
- spawning a command under an independently owned PTY;
- attach, input, resize, output, detach, status, list, and terminate operations,
  each bounded by a deadline (`KPB_DEFAULT_TIMEOUT_MILLIS`, 2 s; `list`
  1 s overall across all sessions) with `*_timeout` forms taking an explicit
  bound and `kpb_list_with_options` reporting unreachable sessions;
- `kpb_terminate_expect` (identity-bound terminate), `kpb_check_runtime`, `kpb_session_socket_path`, `kpb_read_cwd_now` (and `kpb_read_cwd_now_ex`, which says when the directory was removed), `kpb_spawn_timeout`,
  `kpb_read_boot_id`/`kpb_read_start_ticks`, and the reaped archive
  (`kpb_list_reaped`, `kpb_reaped_path`), all additive;
- read-only observation and resumable replay through `kpb_observe` and
  `kpb_attach_with_options`; the original `kpb_attach` is unchanged and remains
  the version 1 entry point;
- bounded input queuing so large pastes respect PTY backpressure without loss;
- batched PTY output and vectored frame writes, bounded by `KPB_IO_CHUNK`;
- versioned framed communication over owner-only Unix sockets;
- a bounded replay journal for reconstructing a newly attached terminal;
- an optional durable transcript of session output.

`kpb_spawn` preserves caller descriptors for the command subject to normal
`FD_CLOEXEC` handling, as a direct spawn would, but the persistent broker
closes its own copies after the PTY child is created. The command starts with
an empty signal mask, and dispositions the broker changes for its own lifecycle
are restored to their conventional defaults.

Runtime and session directories must be absolute, real directories owned by
the current user. They are forced to mode `0700`; sockets and metadata are
private. Session IDs are conservative single path components. On Linux the
broker validates each connecting peer and the client validates the broker with
`SO_PEERCRED`, so neither side accepts a socket owned by another user.

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
  forced back to `0600`, brought within the configured limit immediately, and
  then continued rather than replaced, so a recovered pane keeps its bounded
  history. Transcript failures are non-fatal: the broker closes the log and
  the pane keeps running.
- The pane size is recorded alongside the output, because full-screen programs
  address the cursor by row and column and a replay cannot place their text
  without it. The record is a private APC that terminals ignore:
  `ESC _ kilix-transcript;rows=R;cols=C ESC \`. One is written when the
  transcript opens and another whenever the size changes. After a rotation the
  file starts with one carrying `;rotated=1`, which also tells a reader that
  the session's beginning was dropped. Records count against the size limit.

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

Selector validation preserves and checks the selector supplied by the caller.
It does not authenticate the user's original intent. A consumer must preserve
that intent through its own request path and qualify its behavior separately;
local broker tests do not establish end-to-end consumer acceptance.

This library protects panes from frontend loss. It cannot survive the broker
itself being sent `SIGKILL`, the OOM killer, a reboot, or loss of the user
session. Threads are deliberately not used as a lifetime boundary because all
threads die with their process.

A session lost that way does not leave permanent garbage, though: the session
directory's metadata records the broker's pid, boot id and process start time,
and a later `list` or a respawn under the same ID removes the directory once the
broker is **provably** gone: its pid does not exist, or the recorded boot is not
this boot (the pid then belongs to a machine that no longer exists), or the pid
exists but its start time differs (the number was reused). Metadata written
before those fields existed keeps the pid-only rule, and anything short of proof
- missing or unreadable metadata, a pid that exists, an identity that cannot be
read now - leaves the directory untouched, so a live session is never reaped.
Proof, removal, and recreation are serialised through a lock on the sessions
directory, so two concurrent respawns of the same ID cannot reap each other's
fresh session. Reaping keeps the journal; see "Reaped journals". It assumes
the lister shares the broker's pid namespace and `/proc`.
