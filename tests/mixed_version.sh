#!/usr/bin/env bash
#
# Protocol compatibility across a build boundary.
#
# A broker process outlives its frontend by design, and this project is built
# to one fixed path with no generations, so an update replaces the client
# binary while brokers from the previous build keep running.  Mixed versions
# are therefore the normal case during an update, not an edge case, and both
# directions have to work.
#
# This test is deliberately not part of `make test`: it builds a second
# revision from git, which the ordinary suite must not require.
#
#   tests/mixed_version.sh [BASE_REVISION]
#
# BASE_REVISION defaults to the merge base with main, i.e. the last revision
# that predates this branch's protocol work.
set -u

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
base=${1:-$(git -C "$root" merge-base HEAD main)}
work=$(mktemp -d /tmp/kpbmv.XXXXXX)
old_tree="$work/old"
failures=0

cleanup() {
    for runtime in "$work"/rt-*; do
        [ -d "$runtime" ] || continue
        for binary in "$root/build/kitty-pty-broker" "$old_tree/build/kitty-pty-broker"; do
            [ -x "$binary" ] || continue
            "$binary" --runtime-dir "$runtime" list 2>/dev/null \
                | awk '{print $1}' \
                | while read -r id; do
                    [ -n "$id" ] && "$binary" --runtime-dir "$runtime" kill "$id" >/dev/null 2>&1
                done
        done
    done
    sleep 0.5
    git -C "$root" worktree remove --force "$old_tree" >/dev/null 2>&1
    rm -rf -- "$work"
}
trap cleanup EXIT

report() {
    if [ "$1" = pass ]; then
        printf 'pass  %s\n' "$2"
    else
        printf 'FAIL  %s\n' "$2"
        failures=$((failures + 1))
    fi
}

echo "base revision: $base"
git -C "$root" worktree add --detach "$old_tree" "$base" >/dev/null 2>&1 || {
    echo "could not create a worktree at $base" >&2
    exit 2
}
make -C "$root" --silent >/dev/null || { echo "current build failed" >&2; exit 2; }
make -C "$old_tree" --silent >/dev/null || { echo "base build failed" >&2; exit 2; }

new_cli="$root/build/kitty-pty-broker"
old_cli="$old_tree/build/kitty-pty-broker"
echo "new: $("$new_cli" version)"
echo "old: $("$old_cli" version)"

# Start a pane under $1 and leave it running with nothing attached.
#
# `run` both starts the session and attaches to it, so its stdin is /dev/null:
# the attach ends immediately at EOF and the pane keeps running with its
# read-write slot free, which is what the attaches below need.
start_session() {
    local binary=$1 runtime=$2 id=$3
    setsid "$binary" --runtime-dir "$runtime" run --id "$id" \
        -- /bin/sh -c 'stty -echo; printf "MIXED_OK:"; sleep 20' \
        </dev/null >/dev/null 2>&1 &
    local attempt
    for attempt in $(seq 1 40); do
        if "$binary" --runtime-dir "$runtime" status "$id" 2>/dev/null | grep -q detached; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

# --- new client against an old broker: the update-time case ----------------
rt_old=$work/rt-old
mkdir -p "$rt_old"; chmod 700 "$rt_old"
if start_session "$old_cli" "$rt_old" pane; then
    out=$(timeout 5 "$new_cli" --runtime-dir "$rt_old" attach pane </dev/null 2>/dev/null || true)
    case "$out" in
        *MIXED_OK:*) report pass "new client attaches an old broker" ;;
        *)           report fail "new client attaches an old broker (got: $out)" ;;
    esac

    # A capability the old broker does not have must fail promptly and
    # legibly, never hang and never appear to succeed.
    err=$(timeout 5 "$new_cli" --runtime-dir "$rt_old" observe pane </dev/null 2>&1 >/dev/null)
    status=$?
    if [ "$status" -eq 124 ]; then
        report fail "new client observe against an old broker hung"
    elif [ "$status" -eq 0 ]; then
        report fail "new client observe against an old broker reported success"
    else
        case "$err" in
            *"protocol error"*) report pass "new client observe against an old broker is refused clearly" ;;
            *)                  report fail "new client observe against an old broker: unclear error ($err)" ;;
        esac
    fi
else
    report fail "could not start a session under the old broker"
fi

# --- old client against a new broker ---------------------------------------
rt_new=$work/rt-new
mkdir -p "$rt_new"; chmod 700 "$rt_new"
if start_session "$new_cli" "$rt_new" pane; then
    out=$(timeout 5 "$old_cli" --runtime-dir "$rt_new" attach pane </dev/null 2>/dev/null || true)
    case "$out" in
        *MIXED_OK:*) report pass "old client attaches a new broker" ;;
        *)           report fail "old client attaches a new broker (got: $out)" ;;
    esac

    # And it keeps working while observers are watching the same pane.
    timeout 4 "$new_cli" --runtime-dir "$rt_new" observe pane </dev/null >/dev/null 2>&1 &
    watcher=$!
    sleep 1
    out=$(timeout 5 "$old_cli" --runtime-dir "$rt_new" attach pane </dev/null 2>/dev/null || true)
    wait "$watcher" 2>/dev/null || true
    case "$out" in
        *MIXED_OK:*) report pass "old client is undisturbed by an observer" ;;
        *)           report fail "old client is undisturbed by an observer (got: $out)" ;;
    esac
else
    report fail "could not start a session under the new broker"
fi

if [ "$failures" -eq 0 ]; then
    echo "all mixed-version checks passed"
    exit 0
fi
echo "$failures mixed-version check(s) failed" >&2
exit 1
