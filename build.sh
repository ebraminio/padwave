#!/usr/bin/env bash
# Build padwave: one source, one binary. The wire static_asserts are part of
# dualsense.hh, so compiling the app validates them. -Weverything keeps the
# remaining warnings honest; time reports the compile cost.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-clang}"
PKGS="libavformat libavcodec libswresample libavutil libavfilter"
CFLAGS="$(pkg-config --cflags $PKGS)"
LIBS="$(pkg-config --libs $PKGS) -lopus -lm"
# libbluetooth-dev provides libbluetooth; BlueZ's pkg-config module is only
# named "bluez" and emits this same -lbluetooth.
if [ "$(uname -s)" = "Linux" ]; then LIBS="$LIBS -lbluetooth"; fi

echo "==> padwave"
"$CXX" -std=c++23 -Wno-c23-extensions -O3 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -Weverything -Werror -Wno-unsafe-buffer-usage -Wno-c++98-compat-pedantic -Wno-padded -Wno-format-nonliteral \
  -I. $CFLAGS -o padwave padwave.cc $LIBS
