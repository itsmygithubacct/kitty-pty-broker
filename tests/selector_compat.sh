#!/usr/bin/env bash
# New attachment-bound selector end against the last committed receiver.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d /tmp/selector-compat.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
base=7464b038dc42e92cca42199a42da1670dc730473
mkdir "$work/old"
git -C "$root" archive "$base" | tar -x -C "$work/old"
make -s -C "$work/old"
make -s -C "$root"
python3 "$root/tests/test_selector.py" "$root/build/kitty-pty-broker" \
  "$work/old/build/kitty-pty-broker" \
  RealBroker.test_older_receiver_refuses_required_attachment_without_fallback -v
