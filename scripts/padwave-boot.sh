#!/bin/sh
# Boot padwave.
#
# Mix resolves the dependency SCMs (and therefore needs Hex) before it loads
# the app, even when nothing needs compiling. A corrupt Hex archive therefore
# aborts the boot before Bandit binds the port, and systemd turns that into a
# restart loop with no web UI. Repair Hex first, then exec the app so systemd
# tracks a single process.
set -e

cd "$(dirname "$0")/.." || exit 1

if ! mix hex.info >/dev/null 2>&1; then
  mix local.hex --force >/dev/null 2>&1 || true
fi

exec mix run --no-halt --no-compile
