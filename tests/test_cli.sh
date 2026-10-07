#!/bin/sh
set -u

cli=$1
runtime=/tmp/kitty-pty-broker-cli-parser-not-created

expect_usage() {
    "$@" >/dev/null 2>&1
    status=$?
    if [ "$status" -ne 2 ]; then
        printf 'expected usage exit 2, got %s: ' "$status" >&2
        printf '%s ' "$@" >&2
        printf '\n' >&2
        exit 1
    fi
}

for value in '' -1 +1 ' 1' 1x 18446744073709551615; do
    expect_usage "$cli" --runtime-dir "$runtime" run --journal-limit "$value" -- /bin/true
    expect_usage "$cli" --runtime-dir "$runtime" run --transcript-limit "$value" -- /bin/true
done

for cursor in -1:0 +1:0 ' 1:0' 1:-1 1:+1 1: 1:0x :1; do
    expect_usage "$cli" --runtime-dir "$runtime" attach pane --resume "$cursor"
    expect_usage "$cli" --runtime-dir "$runtime" observe pane --from "$cursor"
done

printf 'kitty-pty-broker CLI parser tests passed\n'

# --- behaviour against real brokers ----------------------------------------
#
# Every command runs under `env -i` with an explicit scratch runtime: nothing
# here may reach a runtime named by KITTY_PTY_BROKER_RUNTIME or the user's own.
# Each broker started is recorded so it can be killed, and a stopped broker is
# always continued first.
cli=$(cd "$(dirname "$cli")" && pwd)/$(basename "$cli")
scratch=$(mktemp -d /tmp/kpbt.XXXXXX) || exit 1
rt=$scratch/rt
mkdir "$rt" && chmod 700 "$rt" || exit 1
pidfile=$scratch/pids
: > "$pidfile"
fail_count=0

cleanup() {
    for pid in $(cat "$pidfile" 2>/dev/null); do kill -CONT "$pid" 2>/dev/null; done
    for id in $(kpb --runtime-dir "$rt" list 2>/dev/null | cut -f1); do
        kpb --runtime-dir "$rt" kill "$id" >/dev/null 2>&1
    done
    sleep 0.3
    for pid in $(cat "$pidfile" 2>/dev/null); do kill -KILL "$pid" 2>/dev/null; done
    rm -rf -- "$scratch"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

kpb() { env -i PATH="$PATH" "$cli" "$@"; }

pass() { printf 'pass  %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1" >&2; fail_count=$((fail_count + 1)); }
check() { # check DESCRIPTION CONDITION-EXIT-STATUS
    if [ "$2" -eq 0 ]; then pass "$1"; else fail "$1"; fi
}
now_ms() { date +%s%N | cut -c1-13; }
json_field() { # json_field NAME < json   (a scalar, string or number)
    sed -n "s/.*\"$1\":\(\"[^\"]*\"\|[^,}]*\).*/\1/p" | head -n 1
}

# Start a detached session: `run` attaches, reads EOF on stdin and leaves the
# pane running.  Records the broker pid; prints it.
start() { # start ID COMMAND...
    id=$1; shift
    kpb --runtime-dir "$rt" run --id "$id" -- "$@" </dev/null >/dev/null 2>&1
    pid=$(kpb --runtime-dir "$rt" status "$id" --json | json_field broker_pid)
    [ -n "$pid" ] || return 1
    printf '%s\n' "$pid" >> "$pidfile"
    printf '%s' "$pid"
}

# -h / --help / help: usage on stdout, exit 0, nothing on stderr.
for form in "--help" "-h" "help" "--runtime-dir $rt --help" "--timeout 1 -h" "list --help" "kill --help"; do
    # shellcheck disable=SC2086
    out=$(kpb $form 2>"$scratch/err"); status=$?
    [ "$status" -eq 0 ] && [ ! -s "$scratch/err" ] && printf '%s' "$out" | grep -q -- '--timeout SECONDS' &&
        printf '%s' "$out" | grep -q 'XDG_RUNTIME_DIR' && printf '%s' "$out" | grep -q 'reaped path ID'
    check "help: $form" $?
done
kpb >/dev/null 2>&1; [ $? -eq 2 ]; check "no command is a usage error" $?

# --timeout is seconds, 0.1 to 60, digits and one point only.
for value in 0.05 61 60.5 abc '' ' 1' 1e1 -1 +1 nan inf 0x1 1.2.3 .; do
    kpb --timeout "$value" --runtime-dir "$rt" list >/dev/null 2>&1; status=$?
    [ "$status" -eq 2 ] || { fail "--timeout '$value' should be a usage error (got $status)"; }
done
for value in 0.1 0.5 2 60 .5 1.; do
    kpb --timeout "$value" --runtime-dir "$rt" list >/dev/null 2>&1; status=$?
    [ "$status" -eq 0 ] || { fail "--timeout '$value' should be accepted (got $status)"; }
done
kpb --timeout >/dev/null 2>&1; [ $? -eq 2 ]; check "--timeout needs a value" $?
kpb --runtime-dir "$rt" --timeout 1 list >/dev/null 2>&1; a=$?
kpb --timeout 1 --runtime-dir "$rt" list >/dev/null 2>&1; b=$?
[ "$a" -eq 0 ] && [ "$b" -eq 0 ]; check "--runtime-dir and --timeout in either order" $?

# A runtime that does not exist is an error, not an empty listing.
missing=$scratch/not-there
expected="kitty-pty-broker: runtime directory does not exist: $missing (set KITTY_PTY_BROKER_RUNTIME or pass --runtime-dir)"
for command in "list" "list --json" "status x" "kill x" "attach x" "observe x" "tui" "reaped"; do
    # shellcheck disable=SC2086
    err=$(kpb --runtime-dir "$missing" $command 2>&1 >/dev/null </dev/null); status=$?
    [ "$status" -eq 1 ] && [ "$err" = "$expected" ]
    check "missing runtime: $command" $?
done
[ ! -e "$missing" ]; check "a missing runtime is not created by looking at it" $?
empty=$scratch/empty; mkdir "$empty"; chmod 700 "$empty"
out=$(kpb --runtime-dir "$empty" list 2>&1); status=$?
[ "$status" -eq 0 ] && [ -z "$out" ]; check "a runtime with no sessions/ is an empty listing" $?
out=$(kpb --runtime-dir "$empty" list --json 2>&1); [ "$out" = "[]" ]; check "an empty runtime lists []" $?
ln -s "$empty" "$scratch/link"
err=$(kpb --runtime-dir "$scratch/link" list 2>&1 >/dev/null); status=$?
[ "$status" -eq 1 ] && printf '%s' "$err" | grep -q 'not safe to use'; check "a symlinked runtime is refused" $?
: > "$scratch/file"
err=$(kpb --runtime-dir "$scratch/file" list 2>&1 >/dev/null); status=$?
[ "$status" -eq 1 ] && printf '%s' "$err" | grep -q 'not safe to use'; check "a non-directory runtime is refused" $?

# An over-long socket path is named, and nothing is created.
long=$scratch/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
err=$(kpb --runtime-dir "$long" run --id toolong -- /bin/true 2>&1 >/dev/null </dev/null); status=$?
path="$long/sessions/toolong/control.sock"
[ "$status" -eq 1 ] && [ "$err" = "kitty-pty-broker: socket path too long (${#path} bytes, limit 107): $path" ]
check "path too long is named exactly" $?
[ ! -e "$long" ]; check "a refused spawn creates no directories" $?
mkdir -p "$scratch/short" && chmod 700 "$scratch/short"
kpb --runtime-dir "$scratch/short/$(printf 'b%.0s' 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70)" \
    run -- /bin/true >/dev/null 2>&1 </dev/null
[ -z "$(ls -A "$scratch/short")" ]; check "nothing is created inside an existing parent either" $?

# A stopped broker neither hangs the CLI nor hides a healthy session.
wedged_pid=$(start wedged sleep 300) && healthy_pid=$(start healthy sleep 300)
[ -n "$wedged_pid" ] && [ -n "$healthy_pid" ]; check "sessions start" $?
kill -STOP "$wedged_pid"
t0=$(now_ms)
out=$(kpb --runtime-dir "$rt" --timeout 0.5 list 2>"$scratch/err"); status=$?
elapsed=$(( $(now_ms) - t0 ))
[ "$status" -eq 0 ] && [ "$elapsed" -lt 1800 ] && printf '%s' "$out" | grep -q '^healthy	' &&
    ! printf '%s' "$out" | grep -q wedged && grep -qx 'kitty-pty-broker: list: wedged: timeout' "$scratch/err"
check "list with a stopped broker: bounded, exit 0, healthy listed, warning on stderr ($elapsed ms)" $?
out=$(kpb --runtime-dir "$rt" --timeout 0.5 list --all 2>/dev/null)
printf '%s' "$out" | grep -q "^wedged	unreachable	pid=-	error=timeout$" && printf '%s' "$out" | grep -q '^healthy	'
check "list --all names the unreachable session" $?
out=$(kpb --runtime-dir "$rt" --timeout 0.5 list --json 2>/dev/null)
! printf '%s' "$out" | grep -q wedged && printf '%s' "$out" | grep -q '"id":"healthy"' && ! printf '%s' "$out" | grep -q reachable
check "plain list --json omits unreachable sessions and the reachable field" $?
out=$(kpb --runtime-dir "$rt" --timeout 0.5 list --json --all 2>/dev/null)
printf '%s' "$out" | grep -q '{"id":"wedged","reachable":false,"error":"timeout"}' &&
    printf '%s' "$out" | grep -q '"id":"healthy".*"reachable":true}'
check "list --all --json carries reachable and error" $?
t0=$(now_ms)
kpb --runtime-dir "$rt" --timeout 0.5 status wedged >/dev/null 2>"$scratch/err"; status=$?
elapsed=$(( $(now_ms) - t0 ))
[ "$status" -eq 1 ] && [ "$elapsed" -lt 1800 ]; check "status on a stopped broker is bounded ($elapsed ms)" $?
t0=$(now_ms)
err=$(kpb --runtime-dir "$rt" --timeout 0.5 kill wedged 2>&1 >/dev/null); status=$?
elapsed=$(( $(now_ms) - t0 ))
[ "$status" -eq 1 ] && [ "$elapsed" -lt 1800 ] &&
    [ "$err" = "kitty-pty-broker: kill session: timed out; the broker may still act on the request" ]
check "kill on a stopped broker says it may still act ($elapsed ms)" $?
t0=$(now_ms)
kpb --runtime-dir "$rt" --timeout 0.5 attach wedged </dev/null >/dev/null 2>"$scratch/err"; status=$?
elapsed=$(( $(now_ms) - t0 ))
[ "$status" -eq 1 ] && [ "$elapsed" -lt 1800 ] && grep -q 'timed out' "$scratch/err"
check "attach to a stopped broker is bounded ($elapsed ms)" $?
kill -CONT "$wedged_pid"

# Several stopped brokers cost one deadline.
for n in 1 2 3 4; do
    p=$(start "stuck$n" sleep 300) && kill -STOP "$p"
done
t0=$(now_ms)
out=$(kpb --runtime-dir "$rt" --timeout 0.6 list --all 2>/dev/null)
elapsed=$(( $(now_ms) - t0 ))
[ "$elapsed" -lt 1500 ] && [ "$(printf '%s\n' "$out" | grep -c unreachable)" -eq 4 ]
check "four stopped brokers cost one deadline ($elapsed ms, not 2400)" $?
for p in $(cat "$pidfile"); do kill -CONT "$p" 2>/dev/null; done
for n in 1 2 3 4; do kpb --runtime-dir "$rt" kill "stuck$n" >/dev/null 2>&1; done

# cwd is where the session started; cwd_now is where its command is.
mkdir "$scratch/started-here"
( cd "$scratch/started-here" && kpb --runtime-dir "$rt" run --id cwdtest -- sh -c 'cd /usr && sleep 300' </dev/null >/dev/null 2>&1 )
attempt=0; now=""
while [ "$attempt" -lt 50 ]; do
    json=$(kpb --runtime-dir "$rt" status cwdtest --json)
    now=$(printf '%s' "$json" | json_field cwd_now)
    [ "$now" = '"/usr"' ] && break
    attempt=$((attempt + 1)); sleep 0.1
done
printf '%s\n' "$(printf '%s' "$json" | json_field broker_pid)" >> "$pidfile"
[ "$now" = '"/usr"' ] && [ "$(printf '%s' "$json" | json_field cwd)" = "\"$scratch/started-here\"" ]
check "status --json: cwd is the start directory, cwd_now the live one" $?
printf '%s' "$json" | grep -q '"boot_id":"[0-9a-f-]*"' && printf '%s' "$json" | grep -q '"start_ticks":[0-9][0-9]*'
check "status --json carries boot_id and start_ticks" $?
kpb --runtime-dir "$rt" list --json | grep -q '"cwd_now":"/usr"'; check "list --json items carry cwd_now" $?

# A dead broker's journal is kept, listed and findable.
bpid=$(start dies.badly sh -c 'printf KEEP_THIS_SCREEN; sleep 300')
sleep 0.5
cpid=$(kpb --runtime-dir "$rt" status dies.badly --json | json_field child_pid)
started=$(kpb --runtime-dir "$rt" status dies.badly --json | json_field started_millis)
kill -KILL "$bpid"; kill -KILL "$cpid" 2>/dev/null
attempt=0; while [ -e "/proc/$bpid" ] && [ "$attempt" -lt 50 ]; do attempt=$((attempt + 1)); sleep 0.1; done
[ ! -e "/proc/$bpid" ]; check "the killed broker is gone (reaped by init)" $?
kpb --runtime-dir "$rt" list >/dev/null 2>&1
[ ! -e "$rt/sessions/dies.badly" ]; check "list reaps the corpse" $?
journal=$(kpb --runtime-dir "$rt" reaped path dies.badly); status=$?
[ "$status" -eq 0 ] && [ "$journal" = "$rt/reaped/dies.badly.$started.journal" ] && grep -q KEEP_THIS_SCREEN "$journal"
check "reaped path prints the archived journal" $?
out=$(kpb --runtime-dir "$rt" reaped); printf '%s' "$out" | grep -q "^dies.badly	started=$started	bytes=[0-9]*	$journal$"
check "reaped lists it" $?
out=$(kpb --runtime-dir "$rt" reaped --json)
printf '%s' "$out" | grep -q "\"id\":\"dies.badly\",\"started_millis\":$started,\"reaped_millis\":[0-9]*,\"journal_bytes\":[0-9]*,\"journal\":\"$journal\",\"meta\":\"$rt/reaped/dies.badly.$started.meta\"}"
check "reaped --json" $?
err=$(kpb --runtime-dir "$rt" reaped path nobody 2>&1 >/dev/null); status=$?
[ "$status" -eq 1 ] && [ "$err" = "kitty-pty-broker: reaped: no archived journal for nobody" ]
check "reaped path for an unknown id fails" $?
[ "$(stat -c %a "$rt/reaped")" = 700 ] && [ "$(stat -c %a "$journal")" = 600 ]; check "reaped/ is 0700 and journals 0600" $?

# A process holding the sessions lock (here: stopped while holding it) must not
# hang `list`: reaping is skipped when the lock cannot be taken in time.
if command -v flock >/dev/null 2>&1; then
    lockrt=$scratch/lockrt
    mkdir -p "$lockrt/sessions/dead" && chmod 700 "$lockrt" "$lockrt/sessions"
    printf 'version=1\nid=dead\nbroker_pid=%s\nchild_pid=1\nstarted_millis=1\n' 2147483000 > "$lockrt/sessions/dead/metadata"
    # flock(1) runs `sleep` as its child, and it is the child that keeps the
    # descriptor - and so the lock - open; both are stopped and both recorded.
    flock "$lockrt/sessions" sleep 30 &
    holder=$!
    sleep 0.3
    holder_child=$(pgrep -P "$holder")
    printf '%s\n%s\n' "$holder" "$holder_child" >> "$pidfile"
    kill -STOP "$holder" $holder_child 2>/dev/null
    t0=$(now_ms)
    timeout 5 env -i PATH="$PATH" "$cli" --runtime-dir "$lockrt" --timeout 0.2 list >/dev/null 2>&1; status=$?
    elapsed=$(( $(now_ms) - t0 ))
    [ "$status" -eq 0 ] && [ "$elapsed" -lt 2000 ] && [ -d "$lockrt/sessions/dead" ]
    check "list does not wait for a held sessions lock, and skips reaping ($elapsed ms, exit $status)" $?
    kill -CONT "$holder" $holder_child 2>/dev/null; kill -KILL "$holder" $holder_child 2>/dev/null; wait "$holder" 2>/dev/null
    kpb --runtime-dir "$lockrt" list >/dev/null 2>&1
    [ ! -d "$lockrt/sessions/dead" ]; check "once the lock is free the corpse is reaped" $?
else
    printf 'skip  sessions-lock test (flock(1) not available)\n'
fi

[ "$fail_count" -eq 0 ] || { printf '%s CLI check(s) failed\n' "$fail_count" >&2; exit 1; }
printf 'kitty-pty-broker CLI behaviour tests passed\n'
