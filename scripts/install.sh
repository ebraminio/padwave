#!/usr/bin/env bash
# Install / update padwave on the Raspberry Pi.
# Run this on the Pi, from the project root.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

echo "==> building (MIX_ENV=prod)"
export MIX_ENV=prod
mix local.hex --force >/dev/null
mix local.rebar --force >/dev/null
mix deps.get >/dev/null
mix compile

echo "==> systemd user unit"
mkdir -p "$HOME/.config/systemd/user"
install -m 0644 "$ROOT/scripts/padwave.service" "$HOME/.config/systemd/user/padwave.service"
systemctl --user daemon-reload
systemctl --user enable padwave >/dev/null
systemctl --user restart padwave

sleep 3
systemctl --user --no-pager --lines=15 status padwave || true

echo
echo "padwave is running:  http://$(hostname -I | awk '{print $1}'):8787"
