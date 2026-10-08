# Literal selectors and conditional end transport

The shared evaluator is `kpb_resolve_selector` in `src/selector.inc`. The C API
takes one discriminator: exact opaque ID, literal ID prefix, or a sequence of
startup words. ID characters are ASCII letters, digits, dot, underscore and
hyphen, 1–64 characters; an exact ID cannot be `.` or `..`. Prefixes can have any
length in that range, including a single dot. Matching is case-sensitive.

Startup text is original argv joined by one space, then split on ASCII space,
tab, newline, carriage return, vertical tab and form feed. The selector must
equal a contiguous sequence of whole words. Quotes and punctuation stay
literal; there is no shell parsing, case folding, fuzzy matching, numeric
normalization, or inference about the current process. Words cannot contain
ASCII whitespace, ASCII control characters or NUL. At most 64 words and
4096 UTF-8 bytes including separators. Attachment is optional and conjunctive.

`resolve --id ID --json`, `resolve --id-prefix PREFIX --json`, and repeated
`--command-word WORD` flags expose the evaluator; `--attached` or `--detached`
adds the predicate. Unknown flags, multiple discriminators, repeated constraints
and invalid values refuse. Kilix/Needle JSON adapters also reject unknown and
duplicate fields.

The new census is read-only. It enumerates at most 1024 session entries,
queries statuses concurrently under one deadline, pins private directories,
and reads complete bounded regular metadata without following symlinks.
It never calls the legacy list or reap. Socketless, provisional, unreachable,
unsafe and malformed entries remain in the census. Invalid entry names,
enumeration errors or capacity exhaustion make the census incomplete.
Unknown startup evidence cannot prove a nonmatch. A complete recorded argv
can prove a nonmatch, including on an unreachable row; it cannot authorize an
unreachable target. The aggregate argv/cwd truncation flag is conservative.
All predicates are applied before counting; the caller's session is included.
A unique match with no possible alternatives must also be reachable.

The receipt `kitty-pty-selector/v1` includes complete/counts, at most 32 candidate
summaries, a truncation flag and the unique incarnation when actionable.
Counts include rows omitted from display. Caller context reads the environment:
`source=environment`, `authenticated=false`, valid/missing/malformed, and the
valid ID or null. No caller override is accepted.

Resolve is only a preview. The raw `end` transport requires the original
selector and `--resolved-id ID --expect-started MILLIS` from the frontend's
durably reserved binding. It re-evaluates the selector, refuses changed binding,
counts the caller before refusing own-session ends, revalidates the candidate,
and sends one termination. Kilix owns operation admission/retries/verification;
raw broker end is not an independently durable agent API. Agents must use
Kilix end or Needle end, never this raw transport.

The existing empty termination and eight-byte expected-start payload remain
unchanged. With an attachment predicate, the new 16-byte payload carries
expected start, attached=0/1 and a zero reserved word. The receiver compares
both conditions at acceptance. Older receivers answer invalid request and do
nothing; the client reports cannot_bind and never falls back. Without an
attachment predicate, eight-byte expected-start binding is still mandatory.

A successful ACK means accepted termination, not verified process absence.
Send flags distinguish no send, may-have-sent/lost reply and receiver rejection.
Negative ACKs identify a sent request that was refused. Timeout after sending
remains uncertain even if the process later disappears.

This is conservative uniqueness in a sampled census, not a globally atomic
snapshot. Creation, removal and attachment can change between observations.
There are no new global transition locks, nonces or attachment ABA guarantees.
The existing millisecond start value can collide on rapid ID reuse. Same-user
metadata/environment/IPC are not authenticated original user intent. A fabricated
exact selector can still choose another valid session; this remains an explicit
provenance limitation, tested without claiming it is repaired.

Run `make test compatibility sanitize` in a disposable private environment.
`tests/test_selector.py CURRENT [OLD]` covers real receivers and the older
receiver condition. Historical exact-ID compatibility suites are retained.
