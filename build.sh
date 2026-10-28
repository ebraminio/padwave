#!/usr/bin/env bash
# Build padwave: one source, one binary. The wire static_asserts are part of
# padwave_wire.hh, so compiling the app validates them. -Weverything keeps the
# remaining warnings honest; time reports the compile cost.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-clang}"
PKGS="libavformat libavcodec libswresample libavutil libavfilter"
CFLAGS="$(pkg-config --cflags $PKGS)"
LIBS="$(pkg-config --libs $PKGS) -lopus -lm"

echo "==> padwave"
"$CXX" -std=c++23 -Wno-c23-extensions -O3 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -Weverything -Wno-unsafe-buffer-usage -Wno-c++98-compat-pedantic -Wno-padded -Wno-format-nonliteral \
  -I. $CFLAGS -o padwave padwave.cc $LIBS
