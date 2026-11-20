#!/usr/bin/env bash
# Build padwave: one source, one binary. The wire static_asserts are part of
# dualsense.hh, so compiling the app validates them. -Weverything keeps the
# remaining warnings honest; time reports the compile cost.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-clang}"
PKGS="libavformat libavcodec libswresample libavutil libavfilter"
MISSING=""
for pkg in $PKGS; do pkg-config --exists "$pkg" || MISSING="$MISSING $pkg"; done
if [ -n "$MISSING" ]; then
  echo "missing build dependencies:$MISSING" >&2
  echo "Raspberry Pi OS/Debian:" >&2
  echo "  sudo apt install clang pkg-config build-essential libavformat-dev libavcodec-dev libswresample-dev libavutil-dev libavfilter-dev libopus-dev libbluetooth-dev" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ] && [ ! -r /usr/include/bluetooth/hci_lib.h ]; then
  echo "missing libbluetooth-dev: sudo apt install libbluetooth-dev" >&2
  exit 1
fi
CFLAGS="$(pkg-config --cflags $PKGS) $(pkg-config --cflags opus | sed 's/-I/-isystem /g')"
LIBS="$(pkg-config --libs $PKGS) -lopus -lm"
PIPEWIRE_FLAGS=""
if pkg-config --exists libpipewire-0.3; then
  PIPEWIRE_FLAGS="-DPADWAVE_HAVE_PIPEWIRE $(pkg-config --cflags libpipewire-0.3 | sed 's/-I/-isystem /g')"
  LIBS="$LIBS $(pkg-config --libs libpipewire-0.3) -pthread"
fi
# libbluetooth-dev provides libbluetooth; BlueZ's pkg-config module is only
# named "bluez" and emits this same -lbluetooth.
if [ "$(uname -s)" = "Linux" ]; then LIBS="$LIBS -lbluetooth"; fi

echo "==> padwave"
"$CXX" -std=c++23 -Wno-c23-extensions -O3 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  $PIPEWIRE_FLAGS \
  -Weverything -Werror -Wno-unsafe-buffer-usage -Wno-c++98-compat-pedantic -Wno-padded -Wno-format-nonliteral \
  -I. $CFLAGS -o padwave padwave.cc $LIBS
