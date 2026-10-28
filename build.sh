#!/usr/bin/env bash
# Build padwave: one source, one binary. The wire static_asserts are part of
# padwave_wire.hh, so compiling the app validates them.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-clang}"
PKGS="libavformat libavcodec libswresample libavutil"
CFLAGS="$(pkg-config --cflags $PKGS)"
LIBS="$(pkg-config --libs $PKGS) -lopus -lm"

echo "==> padwave"
"$CXX" -std=c++23 -Wno-c23-extensions -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra \
  -I. $CFLAGS -o padwave padwave.cc $LIBS
