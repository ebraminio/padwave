#!/usr/bin/env bash
# Install / update padwave on the Raspberry Pi.
# Run this on the Pi, from the project root.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

echo "==> building (MIX_ENV=prod)"
export MIX_ENV=prod
if [ -z "${SKIP_ERLANG_REBUILD:-}" ]; then
  mix local.hex --force >/dev/null
  mix local.rebar --force >/dev/null
  mix deps.get >/dev/null
  mix compile
fi

echo "==> native player + codec helper"
mkdir -p "$HOME/.local/bin"
if command -v cc >/dev/null && pkg-config --exists libavformat libavcodec libswresample 2>/dev/null; then
  # GCC 12 has no #embed; the server includes this generated header instead.
  python3 - "$ROOT/priv/ui/index.html" "$ROOT/scripts/padwave_ui.hh" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as f:
    f.write("#pragma once\n#include <stddef.h>\nstatic const unsigned char kIndexHtml[] = {\n")
    f.write(",".join(str(b) for b in data))
    f.write("\n};\nstatic const size_t kIndexHtmlSize = sizeof(kIndexHtml);\n")
PY
  # Build next to the target and move only on success, so a failed build
  # cannot leave a broken binary or a green "built" line.
  cc -std=c++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2 -I "$ROOT/scripts" \
    -o "$HOME/.local/bin/padwave.new" \
    "$ROOT/scripts/padwave.cc" "$ROOT/scripts/padwave_native.cc" "$ROOT/scripts/padwave_opusdec.cc" \
    "$ROOT/scripts/padwave_input.cc" "$ROOT/scripts/padwave_server.cc" \
    $(pkg-config --cflags --libs libavformat libavcodec libswresample libavutil) -lopus -lz -lm \
    || { rm -f "$HOME/.local/bin/padwave.new"; echo "    build failed" >&2; exit 1; }
  mv "$HOME/.local/bin/padwave.new" "$HOME/.local/bin/padwave"
  rm -f "$HOME/.local/bin/padwave_native" "$HOME/.local/bin/padwave_opusdec"
  echo "    built ~/.local/bin/padwave (native + input + serve + opusdec)"
else
  echo "    skipped: needs libavformat/libavcodec/libswresample and libopus dev packages"
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
