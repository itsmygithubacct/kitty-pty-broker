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
make compatibility
make FUZZ_SECONDS=15 fuzz
make benchmark
```

`compatibility` exercises new-client/old-broker and old-client/new-broker
attachments. The fuzzer drives the framed receive path under ASan and UBSan.
The benchmark reports median live, transcript, graphics-elision, and replay
throughput; run `build/benchmark-broker BYTES SAMPLES` to choose its workload.
Replay throughput deliberately trades a fraction of its ceiling for liveness:
the replay is paced through the client's bounded queue rather than streamed in
one blocking burst, which is what keeps `status` and `kill` answering while a
reattach drains tens of megabytes of history.

The build produces:

- `libkitty-pty-broker.so` and `libkitty-pty-broker.a`
- `kitty-pty-broker`, a CLI built on the public library

The only non-libc dependency is the platform PTY utility library (`libutil` on
Linux).

## CLI

People should use `kilix pty` for caller and own-session checks, identity-bound
kills and verified receipts. Agents use `kilix pty ... --json` or `kilix-needle pty`
with only its exact accepted forms (see `kilix-needle pty --help`), and never this
raw CLI, which has neither own-session nor caller checks. Agents end a session
only on the user's own request for that specific session; for relayed wishes,
end nothing, report and ask the user. If a prefix, title, command or description
matches more than one session, end none: list the matching full IDs and ask
which one. End only a single unambiguous match, using its full ID and
`started_millis` from `kilix pty status ID --json`, then
`kilix pty kill ID --yes --expect-started MILLIS --json`.

```sh
kitty-pty-broker run --id work -- bash
kitty-pty-broker run --id work --transcript ~/.local/state/work.log -- bash
kitty-pty-broker attach work
kitty-pty-broker attach work --resume 0:4096
kitty-pty-broker observe work
kitty-pty-broker observe work --from 0:4096
kitty-pty-broker list --json
kitty-pty-broker list --all
kitty-pty-broker status work --json
kitty-pty-broker kill work
kitty-pty-broker kill work --expect-started 1791346481952
kitty-pty-broker reaped
kitty-pty-broker reaped path work
kitty-pty-broker tui
kitty-pty-broker --runtime-dir /srv/panes --timeout 0.5 list
```

`kitty-pty-broker --help` (also `-h`, `help`, and `COMMAND --help`) prints the
usage to stdout and exits 0.

### Which runtime

Every command works on one runtime directory, chosen in this order:

1. `--runtime-dir DIR` (an absolute path);
2. `$KITTY_PTY_BROKER_RUNTIME`, if it is set to an absolute path;
3. `$XDG_RUNTIME_DIR/kitty-pty-broker`;
4. `/tmp/kitty-pty-broker-UID`.

A host that embeds the broker usually exports `KITTY_PTY_BROKER_RUNTIME` into the
panes it starts, which is why the bare CLI sees that host's sessions from inside
a pane and none from outside it: outside, it falls through to a different
directory. Say which one you mean with `--runtime-dir`.

`list`, `status`, `kill`, `attach`, `observe`, `reaped` and `tui` fail with exit
status 1 when the runtime does not exist:

```
kitty-pty-broker: runtime directory does not exist: PATH (set KITTY_PTY_BROKER_RUNTIME or pass --runtime-dir)
```

A runtime that exists but has no `sessions/` yet is simply an empty listing. A
runtime that is a symlink, is not a directory, or is not owned by you is refused
as unsafe, also with exit status 1. `run` creates the runtime if needed, after
checking that the socket path will fit (below).

### On disk

```
RUNTIME/                         0700, owned by you
    sessions/ID/                 0700, one per live session
        control.sock             0600, the broker's socket
        journal.bin              0600, the replay journal
        metadata                 0600, key=value: pids, started_millis, boot_id, start_ticks
    reaped/                      0700, journals kept from dead sessions (below)
        ID.STARTED_MILLIS.journal
        ID.STARTED_MILLIS.meta
```

The broker also writes `argv_json` (a one-line JSON array, at most 1024 encoded
bytes), `cwd_json` (a JSON string, at most 512 encoded bytes), and
`argv_truncated=0|1` in that same metadata file. The file stays below the
4096-byte identity-read bound; readers never read beyond that bound even if the
file grows after its descriptor check. No extra cleanup names are introduced.
Oversized argv retains a prefix of whole elements; oversized cwd retains a
prefix of whole characters. Either sets `argv_truncated=1`. Valid UTF-8 is kept;
each invalid byte becomes `\ufffd` and also sets the flag (lossy display).

`list --json --all` adds a display-only `recorded` object to unreachable rows:
`{"argv":["sh","-c","sleep 90"],"cwd":"/srv/work","started_millis":123,"truncated":false}`.
These are spawn facts, not the live command or cwd and not a stale-session proof.
Missing fields in older metadata become null (`truncated` defaults to false);
an older record's `started_millis` is retained when present. Malformed or
oversized JSON fields become null and mark the display truncated. Rejected or
missing identity files supply only nulls. Reachable rows and `status` keep their
existing fields and wire format. Older readers ignore the new metadata lines;
the pid + boot_id + start_ticks stale proof is unchanged.

`control.sock`'s full path must fit a Unix socket address, 107 bytes. A longer
one is refused up front, before anything is created:
`kitty-pty-broker: socket path too long (N bytes, limit 107): PATH`. Use a short
runtime such as `/run/user/UID/kpb`.

### Waiting is bounded

No request to a broker, and no lock taken to make one, waits without a deadline.
A broker that is stopped, wedged or swapped out would otherwise hang every
caller that asks about it. The only unbounded wait is for the next frame of an
established `attach`/`observe` stream to *begin* (a quiet pane is not a fault);
once a frame has started arriving, the rest of it must follow within 2 seconds.
`--timeout SECONDS` (a decimal number of **seconds**, 0.1 to 60, given with
`--runtime-dir` before the command, in either order) bounds each wait: the
default is 2 seconds per operation, and 1 second for `list` and `tui`.

A session that does not answer in time, or that fails for any reason other than
"nothing is listening", is **unreachable**. `kill` says which kind of timeout it
was: one that expired after the request was sent says `kill session: timed out;
the broker may still act on the request` and exits 1 - it did not necessarily
fail - while one that expired before the request could be sent (the broker is not
accepting connections) says `timed out before the request was sent ... nothing
was done` and exits 6, and is safe to retry. `run` waits for the sessions lock no
longer than `--timeout`, and for the new broker to come up at least 10 seconds;
if either runs out it says so and exits 1. A spawn that times out on the lock created
nothing. One that times out on the new broker means *the broker did not answer in
time*, not that nothing was started: its command may already be running. If the
broker is only slow it removes the session itself when it finds nobody waiting; if
it stays wedged it remains until it dies, and the metadata the spawn left for it
lets the directory be reaped then, so the ID is not blocked for good.

That metadata is bound to the directory generation the spawn itself created: the
caller keeps a descriptor of the session directory it made (opened under the
sessions lock) and publishes through it, never through the ID's path, so a caller
that is delayed past its broker's end - and past a respawn of the same ID - cannot
put its dead broker's identity into the newer session's directory. When a broker or
the reaper removes a session directory it touches only the known names
(`metadata.tmp`, `metadata.provisional`, `control.sock`, `journal.bin`, `metadata`).
The file that licensed the cleanup - the canonical `metadata`, or, when that is
absent, the provisional copy - is kept until last: every other known name is
unlinked first (each result checked), the directory must then hold nothing but that
proof, and only then are the proof and the directory removed. On any failure or
unknown entry the cleanup stops. "Directory left intact" means the proof is
retained, so a later listing can finish the job; unlinks that came before the
failure may already have happened. The listing shows such a session as unreachable
(reason `system`) until then.

When `attach` or `observe` ends for any reason other than the pane's own exit
(the broker closed the connection, stopped in the middle of a frame, refused, the
terminal went away) it exits 1 and says why, on one line:
`kitty-pty-broker: attach ID: REASON`. Pressing `Ctrl-C`/`SIGTERM` ends an attach
at once even while the pane is streaming output, and an `attach` whose standard
input reaches end of file once the replay is done detaches and exits 0 - input piped
in before that is delivered first, and end of file seen earlier only defers the
detach until the replay is done.

### `list`

`list` asks every session **at the same time** under one overall deadline, so a
runtime with any number of stuck brokers costs one deadline, not one each, and
a stuck broker never hides a healthy one. For each session that did not answer,
`list` prints `kitty-pty-broker: list: ID: REASON` on stderr (`timeout`,
`security`, `protocol`, `system`, `name-too-long`, `invalid`, or the catch-all
`error`) and still exits 0 with the sessions that did
answer on stdout, because callers rely on that. Reaping a corpse during `list`
needs the sessions lock, which is only waited for until `list`'s own deadline: if
another process holds it, `list` skips the reap and still lists, and a later
listing reaps. A directory with nothing listening is not reported as a session.
A broker that accepts the connection and ends it without a word is different: if
its recorded process is gone the directory is reaped, and otherwise the session is
reported as unreachable (`system`) and kept - a live broker is never dropped from
the listing, or from the TUI, for refusing a request. A directory that is proven
stale but cannot be cleared (an unknown entry in it, an unlink that fails) is
reported the same way and left intact, proof included, for a later listing.
It is reaped (below) only when its broker is *proven* gone; a directory whose
metadata is missing or malformed, or whose broker is alive but has lost its
socket, is neither reaped nor listed and stays where it is.

`list --all` also puts unreachable sessions in the listing:

```
ID<TAB>detached<TAB>pid=1234<TAB>COMMAND          reachable (attached or detached)
ID<TAB>unreachable<TAB>pid=-<TAB>error=timeout    did not answer
```

and with `--json`, `{"id":"ID","reachable":false,"error":"timeout"}`, while
reachable items gain `"reachable":true`.

### Status JSON

`status ID --json` and each `list --json` item are one object. The session's own
record keeps its field names, types and meaning; new fields are added, in the
places the table shows (`cwd_now` and `cwd_now_deleted` directly after `cwd`;
`boot_id` and `start_ticks` after `command`; `reachable` last):

| field | meaning |
|---|---|
| `id`, `broker_pid`, `child_pid`, `foreground_pgrp`, `started_millis` | as reported by the broker |
| `journal_bytes`, `journal_epoch`, `attached`, `replay_complete`, `rows`, `columns` | as reported by the broker |
| `cwd` | the directory the session **started** in; fixed for the life of the session |
| `cwd_now` | the command's directory **now**, read by the caller from `/proc/CHILD/cwd`; `null` when unavailable, or when the directory was removed (never the kernel's `"/path (deleted)"` link text, which is not a path) |
| `cwd_now_deleted` | `true` when `cwd_now` is `null` because that directory was removed; `false` otherwise |
| `command` | the command line, truncated to 511 bytes |
| `boot_id` | this machine's boot id, read by the caller; `null` when unavailable |
| `start_ticks` | the broker process's start time (`/proc/PID/stat`, field 22), read by the caller; `null` when unavailable |
| `reachable` | only under `list --all`: `true` |

`boot_id` and `start_ticks` identify the broker process exactly; a caller that
wants a later request to act on this session and no other (a `kill` after a
`list`) can keep them, or `started_millis`, with the id. `cwd_now`, `boot_id` and
`start_ticks` describe the machine and `/proc` the *caller* sees, and `cwd_now`
is read from the `child_pid` the broker reported: it is trusted exactly as far as
a same-user broker is.

### `tui`

`tui` opens the interactive session manager. Detached sessions are sorted first
and unreachable ones last; use the arrow keys or `j`/`k` to select one, Enter to
attach, `o` to observe (read-only, and allowed for a pane that is already
attached; `Ctrl-]` returns to the list), `x` to request termination (with
confirmation), `r` to refresh, and `q` to quit. A session that did not answer is
shown with state `unreachable` and cannot be attached or observed. The header
shows the runtime and the timeout in effect. The interface uses only ANSI
terminal controls and adds no runtime dependency.

### Other commands

`run` starts the broker and attaches the current terminal. Disconnecting that
client leaves the session alive. `kill` is the deliberate termination path;
the broker first sends `SIGTERM` to every process group in the pane's terminal
session, then `SIGKILL` after a bounded grace period.

`observe` attaches read-only. It renders the pane but forwards nothing: keys
are consumed locally and `Ctrl-]` leaves. `SIGWINCH` is ignored, so watching a
pane cannot resize it. Any number of observers may watch a pane that is already
attached, or one that is not attached at all.

Both `attach` and `observe` print `cursor=EPOCH:OFFSET` to stderr on exit. Pass
that back as `--resume` or `--from` to continue where you stopped instead of
repainting from the start of the journal.

### Reaped journals

A broker killed outright (`SIGKILL`, the OOM killer) cannot clean up. Its
session directory is reaped by a later `list` or by a respawn of the same ID -
but not before its journal, the last thing the pane showed, is kept:

- the journal is **moved** (an atomic, constant-time rename within the runtime)
  to `RUNTIME/reaped/ID.STARTED_MILLIS.journal`, and `ID.STARTED_MILLIS.meta`
  beside it holds a copy of the session metadata plus `reaped_millis=`. The
  directory is `0700`, the files `0600`, and nothing is followed through a
  symlink;
- an empty journal is not kept, and if a journal cannot be kept for any reason
  it is deleted as before, so a failure to archive can never leave a corpse that
  blocks its own ID;
- the broker bounds `reaped/` itself, so a standalone install never grows
  without limit: at most 256 MiB and 64 entries, oldest evicted first. An entry
  is a journal (its `.meta` counts toward the bytes, not the count) or a `.meta`
  whose journal is gone, which is counted and evicted like any other so orphans
  cannot pile up. A journal that is itself larger than the byte bound is not kept
  - the bound is a bound. The process doing the reaping
  reads `KITTY_PTY_BROKER_REAPED_MAX_BYTES` and `KITTY_PTY_BROKER_REAPED_MAX_FILES`
  to override them. A host that wants to keep journals longer should compress
  and move them out of `reaped/` itself.

`reaped` lists them (`--json` for a JSON array); `reaped path ID` prints the
path of the newest journal for an ID and exits 1 if there is none. A journal is
raw terminal output: replay it in a terminal, not on your own.

### Exit status

| status | meaning |
|---|---|
| 0 | success (`list` with unreachable sessions is still 0) |
| 1 | failure: missing or unsafe runtime, session not found, timeout, refused spawn, ... |
| 2 | usage error |
| 3 | `kill --expect-started`: refused, the id now names a different session; nothing was done |
| 5 | `kill --expect-started`: the broker predates the check and cannot verify; nothing was done |
| 6 | `kill`: timed out while still connecting, so the request was never sent; nothing was done |

`run` and `attach` return the pane command's own exit status once it exits
(`128+N` if it died from signal `N`), and 1 if they could not attach.

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

## License

MIT
