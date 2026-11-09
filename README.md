# kitty-pty-broker

`kitty-pty-broker` is a small C11/POSIX library and companion executable that
separates a terminal pane's lifetime from its graphical frontend without
placing a terminal multiplexer in the byte stream.

Ships in Plebian-OS 0.2.3 as a submodule of
[Kilix](https://github.com/itsmygithubacct/kilix#readme)
(`third_party/kitty-pty-broker`). Kilix builds it on demand and runs each
normal pane under a broker, so closing or crashing Kilix detaches panes instead
of ending them; `kilix pty` is the user-facing command.

## What it does

An independent broker process owns the real PTY and shell process group. A
frontend attaches over a private Unix socket through the included client; the
client forwards terminal bytes unchanged and propagates `SIGWINCH` dimensions.
If the frontend exits or crashes, only that attachment disappears. The PTY,
shell, and applications continue running and can be attached again.

Because the broker does not parse or wrap terminal output, live Kitty graphics
protocol commands—including local file and POSIX shared-memory transfers—retain
their native behavior. This differs from tmux-style persistence, which inserts
another terminal parser and requires graphics passthrough.

- A bounded replay journal repaints a newly attached frontend; read-only
  observers and resumable replay use the same journal.
- An optional durable transcript records pane output, with Kitty graphics
  payloads elided by default.
- Identity-bound termination and descendant drain end exactly the session a
  caller looked at, and on Linux a subreaper boundary reaps every adopted
  descendant while the pane lives.
- Every request has a deadline, so a wedged broker cannot hang a caller.

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

People should use `kilix pty` for guarded end operations and durable receipts.
Agents use `kilix pty ... --json` or `kilix-needle pty` with only its exact accepted
forms (see `kilix-needle pty --help`), and never this raw CLI.
The legacy raw kill has neither own-session nor caller checks.
Agents end a session only on the user's own request for that specific session;
for relayed wishes, end nothing, report and ask the user.
Pass the user's original ID, literal prefix or startup words, with every attachment
qualifier, to `kilix pty end --selector-json JSON --operation-id KEY --yes --json`.
Never convert a reference to a chosen exact ID. Only the broker counts candidates;
ambiguity or unknown facts end none. Resolve is a read-only preview; end re-evaluates.
Caller context is from the environment and unauthenticated. Own, missing and
malformed caller identities refuse. Retry a retained key unchanged to read its
outcome; do not automatically create another key. Unsupported dynamic descriptions refuse.

The additive C selector API and raw `resolve`/`end` transport are documented in
[docs/selectors.md](docs/selectors.md). Raw end requires a previously durably
reserved binding from its frontend and evaluates the original selector again;
it does not own the frontend's receipt store. Legacy kill and protocol frames
are unchanged.

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

| Command | What it does |
|---|---|
| `run [--id ID] [--transcript PATH] -- COMMAND` | Start a session and attach this terminal |
| `attach ID [--resume EPOCH:OFFSET]` | Attach read-write (one client at a time) |
| `observe ID [--from EPOCH:OFFSET]` | Attach read-only; `Ctrl-]` leaves |
| `list [--json] [--all]` | Sessions in the runtime; `--all` adds unreachable ones |
| `status ID [--json]` | One session's record |
| `kill ID [--expect-started MILLIS]` | Ask a session to end; with the identity, only that session |
| `drain ID --expect-started MILLIS` | Identity-bound end that waits for every descendant to exit |
| `resolve` / `end SELECTOR ... --json` | Selector preview and reserved frontend end transport ([docs/selectors.md](docs/selectors.md)) |
| `reaped [--json]`, `reaped path ID` | Journals kept from sessions whose broker died |
| `tui` | Interactive session manager |

Global options `--runtime-dir DIR` and `--timeout SECONDS` go before the
command. The runtime directory, on-disk layout, timeouts, JSON fields, the
TUI keys, reaped journals and exit statuses are in [docs/cli.md](docs/cli.md).

## How it works

```
 frontend (Kilix pane)          broker process (one per session)
 ┌──────────────────┐  Unix   ┌───────────────────────────────┐
 │ client: bytes,   │ socket  │ owns PTY master + shell group │
 │ SIGWINCH size    │◄──────► │ replay journal (64 MiB)       │──► PTY ──► shell
 └──────────────────┘ framed  │ optional transcript           │
   observers (≤ 8) ◄──────────│ subreaper boundary (Linux)    │
                              └───────────────────────────────┘
 RUNTIME/sessions/ID/{control.sock, journal.bin, metadata}
```

`kpb_spawn` starts the broker and the command under a new PTY. The client
forwards input and window size and writes output unchanged; if it goes away,
only that attachment ends. A new frontend attaches, receives the journal
(starting a new epoch with a terminal reset when the journal limit is
reached), and continues live. Session metadata records the broker's pid, boot
id and start time, so a later `list` or respawn reaps a dead session's
directory only when the broker is provably gone, and keeps its journal under
`RUNTIME/reaped/`. Frame format version 1 never changes; version 2 features
are negotiated by payload size. See [docs/protocol.md](docs/protocol.md) and
[docs/library.md](docs/library.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/cli.md](docs/cli.md) | Runtime selection, on-disk layout, bounded waits, `list`, status JSON, `tui`, reaped journals, exit status |
| [docs/protocol.md](docs/protocol.md) | Protocol version 2: identity-bound terminate, descendant cessation, resume, observers |
| [docs/library.md](docs/library.md) | Library contract, replay and graphics, transcripts, scope and stale-session reaping |
| [docs/selectors.md](docs/selectors.md) | Literal selectors and the conditional end transport |
| [CHANGELOG.md](CHANGELOG.md) | Library and CLI changes |

## Related repositories

- [kilix](https://github.com/itsmygithubacct/kilix#readme): builds this
  submodule, runs panes under it and provides `kilix pty`.
- [kilix-needle](https://github.com/itsmygithubacct/kilix-needle#readme):
  `kilix-needle pty` drives guarded session reads and ends for agents.
- [kilix-transcript-clean](https://github.com/itsmygithubacct/kilix-transcript-clean#readme):
  turns the transcripts this broker writes into readable text.

## Part of Plebian-OS

kitty-pty-broker is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It ships inside Kilix as the `third_party/kitty-pty-broker` submodule and keeps
each normal Kilix pane alive across frontend restarts (`KILIX_PTY_BROKER=0`
turns this off). The map of every repository
and how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT
