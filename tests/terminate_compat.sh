#!/usr/bin/env bash
#
# Identity-bound terminate across a build boundary.
#
#   tests/terminate_compat.sh [BASE_REVISION]
#
# A broker outlives its frontend, so after an update brokers from the previous
# build keep running and are asked to do things they have never heard of.  This
# checks the two halves of that for `kill ID --expect-started MILLIS`:
#
#   - a NEW client against an OLD broker: the old broker answers the 8-byte
#     TERMINATE payload with its existing "invalid request" and does nothing, the
#     client reports "unsupported" (exit 5), and the session SURVIVES - there is
#     no silent fall back to an unconditional kill;
#   - an OLD client against a NEW broker: the empty TERMINATE it sends behaves
#     exactly as before.
#
# BASE_REVISION defaults to 8cf3eb3, the build this work started from (mixed_version.sh
# separately covers the pre-protocol-2 build).  It is fixed, never derived from a
# branch, for the reason mixed_version.sh gives.
set -u

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
base=${1:-8cf3eb3}
work=$(mktemp -d /tmp/kpbtc.XXXXXX)
old_tree="$work/old"
failures=0
pids=""

cleanup() {
    for pid in $pids; do kill -KILL "$pid" 2>/dev/null; done
    git -C "$root" worktree remove --force "$old_tree" >/dev/null 2>&1
    rm -rf -- "$work"
}
trap cleanup EXIT

report() {
    if [ "$1" = pass ]; then printf 'pass  %s\n' "$2"; else printf 'FAIL  %s\n' "$2"; failures=$((failures + 1)); fi
}

echo "base revision: $base"
git -C "$root" worktree add --detach "$old_tree" "$base" >/dev/null 2>&1 || {
    echo "could not create a worktree at $base" >&2; exit 2; }
make -C "$root" BUILD_DIR="$work/new-build" --silent >/dev/null || { echo "current build failed" >&2; exit 2; }
make -C "$old_tree" BUILD_DIR="$work/old-build" --silent >/dev/null || { echo "base build failed" >&2; exit 2; }
new_cli="$work/new-build/kitty-pty-broker"
old_cli="$work/old-build/kitty-pty-broker"
echo "new: $("$new_cli" version)"
echo "old: $("$old_cli" version)"

start_session() { # start_session BINARY RUNTIME ID -> prints the broker pid
    local binary=$1 runtime=$2 id=$3 attempt=0
    setsid "$binary" --runtime-dir "$runtime" run --id "$id" -- /bin/sh -c 'sleep 60' \
        </dev/null >/dev/null 2>&1 &
    while [ "$attempt" -lt 40 ]; do
        attempt=$((attempt + 1))
        if "$binary" --runtime-dir "$runtime" status "$id" >/dev/null 2>&1; then
            pid=$("$binary" --runtime-dir "$runtime" status "$id" --json | sed -n 's/.*"broker_pid":\([0-9]*\).*/\1/p')
            pids="$pids $pid"
            printf '%s' "$pid"
            return 0
        fi
        sleep 0.25
    done
    return 1
}
started_of() { "$new_cli" --runtime-dir "$1" status "$2" --json | sed -n 's/.*"started_millis":\([0-9]*\).*/\1/p'; }

# --- new client, old broker -----------------------------------------------------
rt_old=$work/rt-old; mkdir -p "$rt_old"; chmod 700 "$rt_old"
if start_session "$old_cli" "$rt_old" pane >/dev/null; then
    started=$(started_of "$rt_old" pane)
    if command -v python3 >/dev/null 2>&1; then
        raw=$(python3 - "$rt_old/sessions/pane/control.sock" <<'PY'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
s.sendall(struct.pack(">IHHI", 0x4b504231, 1, 6, 8) + struct.pack(">Q", 1))
h = s.recv(12); n = struct.unpack(">I", h[8:12])[0]
print("frame type %d: %s" % (struct.unpack(">H", h[6:8])[0], s.recv(n).decode()))
PY
)
        printf 'the old broker answers an 8-byte TERMINATE with: %s\n' "$raw"
        [ "$raw" = "frame type 10: invalid request" ]
        [ $? -eq 0 ] && report pass "old broker: 8-byte TERMINATE is answered with ERROR invalid request" \
                     || report fail "old broker answered: $raw"
    fi
    err=$(timeout 5 "$new_cli" --runtime-dir "$rt_old" kill pane --expect-started "$started" 2>&1 >/dev/null); status=$?
    sleep 0.7
    if [ "$status" -eq 5 ] && printf '%s' "$err" | grep -q 'predates identity-checked kill' &&
       "$new_cli" --runtime-dir "$rt_old" status pane >/dev/null 2>&1; then
        report pass "new client vs old broker: kill --expect-started is unsupported (exit 5) and the session survives"
    else
        report fail "new client vs old broker: exit $status, stderr: $err"
    fi
    # A caller that decides to accept the risk can still use the plain kill.
    timeout 5 "$new_cli" --runtime-dir "$rt_old" kill pane >/dev/null 2>&1
    sleep 0.7
    "$new_cli" --runtime-dir "$rt_old" status pane >/dev/null 2>&1 \
        && report fail "new client: plain kill did not end the old broker's session" \
        || report pass "new client: plain kill still ends the old broker's session"
else
    report fail "could not start a session under the old broker"
fi

# --- old client, new broker -----------------------------------------------------
rt_new=$work/rt-new; mkdir -p "$rt_new"; chmod 700 "$rt_new"
if start_session "$new_cli" "$rt_new" pane >/dev/null; then
    timeout 5 "$old_cli" --runtime-dir "$rt_new" kill pane >/dev/null 2>&1; status=$?
    sleep 0.7
    if [ "$status" -eq 0 ] && ! "$new_cli" --runtime-dir "$rt_new" status pane >/dev/null 2>&1; then
        report pass "old client vs new broker: the empty-payload kill is unchanged and ends the session"
    else
        report fail "old client vs new broker: kill exit $status"
    fi
    # And the identity-checked form works on the new broker.
    start_session "$new_cli" "$rt_new" pane2 >/dev/null
    s2=$(started_of "$rt_new" pane2)
    "$new_cli" --runtime-dir "$rt_new" kill pane2 --expect-started "$((s2 + 1))" >/dev/null 2>&1; wrong=$?
    sleep 0.3
    "$new_cli" --runtime-dir "$rt_new" status pane2 >/dev/null 2>&1; alive=$?
    "$new_cli" --runtime-dir "$rt_new" kill pane2 --expect-started "$s2" >/dev/null 2>&1; right=$?
    sleep 0.7
    "$new_cli" --runtime-dir "$rt_new" status pane2 >/dev/null 2>&1; gone=$?
    if [ "$wrong" -eq 3 ] && [ "$alive" -eq 0 ] && [ "$right" -eq 0 ] && [ "$gone" -ne 0 ]; then
        report pass "new client vs new broker: wrong identity refused (3), right identity ends it"
    else
        report fail "new vs new: wrong=$wrong alive=$alive right=$right gone=$gone"
    fi
else
    report fail "could not start a session under the new broker"
fi

if [ "$failures" -eq 0 ]; then echo "all terminate-compatibility checks passed"; exit 0; fi
echo "$failures terminate-compatibility check(s) failed" >&2
exit 1
