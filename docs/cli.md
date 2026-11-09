# CLI reference

The `kitty-pty-broker` command in detail: which runtime directory it uses,
what it keeps on disk, how every wait is bounded, the `list`, `status`, `tui`
and other commands, reaped journals and exit statuses. The agent guidance and
the command summary are in the [README](../README.md#cli).

## Which runtime

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

## On disk

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

## Waiting is bounded

No request to a broker, and no lock taken to make one, waits without a deadline.
A broker that is stopped, wedged or swapped out would otherwise hang every
caller that asks about it. The only unbounded wait is for the next frame of an
established `attach`/`observe` stream to *begin* (a quiet pane is not a fault);
once a frame has started arriving, the rest of it must follow within 2 seconds.
`--timeout SECONDS` (a decimal number of **seconds**, 0.1 to 60, given with
`--runtime-dir` before the command, in either order) bounds each wait: the
default is 2 seconds per operation, and 1 second for `list` and `tui`.
Connect, frame writes and the response share one absolute monotonic deadline,
including partial transfers, interrupted calls and spurious readiness. Selector
end also shares it with its census and final status recheck. Listing drives
partial request writes concurrently, so a blocked writer cannot consume a
separate budget or prevent a healthy row from progressing.

Input, resize and best-effort detach frame writes use the default 2-second
budget. Input/resize transport failure closes that connection, because a
partial frame cannot be followed by another request. Backpressure within that
budget preserves every byte. The broker's existing output queues and their
PTY backpressure/drop policies are unchanged; persistent stream and terminal
output waits are not short request transactions. Writes use `MSG_DONTWAIT`
without changing shared descriptor flags. Where that flag is unavailable,
bounded writes require an already nonblocking descriptor; blocking descriptors
refuse before writing. Private request/reply sockets stay nonblocking. Attach
exports its normal blocking connection where per-call nonblocking writes are
available, and retains nonblocking mode otherwise so input/resize/detach remain
usable. A caller receiving on that fallback connection waits for readability
before calling `kpb_receive`, as for any nonblocking connection.

A session that does not answer in time, or that fails for any reason other than
"nothing is listening", is **unreachable**. `kill` says which kind of timeout it
was: one that expired after the request was sent says `kill session: timed out;
the broker may still act on the request` and exits 1 - it did not necessarily
fail - while one that expired with zero request bytes sent (during connect or
write backpressure) says `timed out before the request was sent ... nothing
was done` and exits 6. A partial write remains uncertain. Durable selector ends
still retain their operation key and never acquire automatic retry permission.
`run` waits for the sessions lock no
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

## `list`

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

## Status JSON

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

## `tui`

`tui` opens the interactive session manager. Detached sessions are sorted first
and unreachable ones last; use the arrow keys or `j`/`k` to select one, Enter to
attach, `o` to observe (read-only, and allowed for a pane that is already
attached; `Ctrl-]` returns to the list), `x` to request termination (with
confirmation), `r` to refresh, and `q` to quit. A session that did not answer is
shown with state `unreachable` and cannot be attached or observed. The header
shows the runtime and the timeout in effect. The interface uses only ANSI
terminal controls and adds no runtime dependency.

## Other commands

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

## Reaped journals

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

## Exit status

| status | meaning |
|---|---|
| 0 | success (`list` with unreachable sessions is still 0) |
| 1 | failure: missing or unsafe runtime, session not found, timeout, refused spawn, ... |
| 2 | usage error |
| 3 | `kill --expect-started`: refused, the id now names a different session; nothing was done |
| 5 | `kill --expect-started`: the broker predates the check and cannot verify; nothing was done |
| 6 | `kill`: timed out before any request byte was sent; nothing was done |

`run` and `attach` return the pane command's own exit status once it exits
(`128+N` if it died from signal `N`), and 1 if they could not attach.
