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
