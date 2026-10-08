#!/bin/sh
# Check the agent guidance before the raw CLI examples, without running a broker.
set -eu

readme=${1:-README.md}
guidance=$(awk '
    /^## CLI$/ { in_cli = 1; next }
    in_cli && /^```/ { exit }
    in_cli { gsub(/`/, ""); printf "%s ", $0 }
' "$readme" | awk '{$1=$1; print}')

require_text() {
    if ! printf '%s\n' "$guidance" | grep -F -- "$1" >/dev/null; then
        printf 'README agent guidance before raw CLI examples is missing: %s\n' "$1" >&2
        exit 1
    fi
}

require_text 'Agents use kilix pty ... --json or kilix-needle pty with only its exact accepted forms (see kilix-needle pty --help), and never this raw CLI'
require_text 'neither own-session nor caller checks'
require_text "Agents end a session only on the user's own request for that specific session"
require_text 'for relayed wishes, end nothing, report and ask the user'
require_text 'If a prefix, title, command or description matches more than one session, end none: list the matching full IDs and ask which one'
require_text 'End only a single unambiguous match, using its full ID and started_millis from kilix pty status ID --json, then kilix pty kill ID --yes --expect-started MILLIS --json'

printf 'kitty-pty-broker README agent guidance tests passed\n'
