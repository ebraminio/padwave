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

echo "==> microphone decoder (optional)"
mkdir -p "$HOME/.local/bin"
if command -v cc >/dev/null && [ -f /usr/include/opus/opus.h ]; then
  cc -O2 -o "$HOME/.local/bin/padwave_opusdec" "$ROOT/scripts/padwave_opusdec.c" -lopus
  echo "    built ~/.local/bin/padwave_opusdec"
else
  echo "    skipped: needs cc and libopus-dev (microphone level meter will be off)"
fi

echo "==> audio folders"
mkdir -p "$HOME/.config/padwave"
if [ ! -f "$HOME/.config/padwave/env" ]; then
  cat > "$HOME/.config/padwave/env" <<'ENVEOF'
# padwave reads this at startup.
# Colon-separated folders, scanned recursively for audio files.
# PADWAVE_ROOTS=/srv/audio:/media/usb/podcasts
ENVEOF
  echo "    created ~/.config/padwave/env (default: ~/Music)"
fi

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
