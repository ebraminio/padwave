#!/usr/bin/env bash
# Build and install padwave on the Pi, then restart the user unit.
# One binary: ./padwave. No Mix, no Hex, no Elixir.
set -euo pipefail

cd "$(dirname "$0")"
ROOT="$(pwd)"

echo "==> build"
"$ROOT/build.sh"

echo "==> install"
install -Dm755 "$ROOT/padwave" "$HOME/.local/bin/padwave"
chmod +x "$ROOT/padwave-boot.sh"

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
install -m 0644 "$ROOT/padwave.service" "$HOME/.config/systemd/user/padwave.service"
systemctl --user daemon-reload
systemctl --user enable padwave >/dev/null
systemctl --user restart padwave

sleep 3
systemctl --user --no-pager --lines=10 status padwave || true

echo
echo "padwave is running:  http://$(hostname -I | awk '{print $1}'):8787"
