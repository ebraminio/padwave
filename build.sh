#!/usr/bin/env bash
# Build padwave: one source, one binary. The wire test is fast, so it is
# compiled and run on every build.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-clang}"
PKGS="libavformat libavcodec libswresample libavutil"
CFLAGS="$(pkg-config --cflags $PKGS)"
LIBS="$(pkg-config --libs $PKGS) -lopus -lm"

echo "==> wire test"
"$CXX" -std=c++23 -Wno-c23-extensions -O2 -fno-exceptions -fno-rtti -I. -o /tmp/padwave_test_wire test_wire.cc
/tmp/padwave_test_wire

echo "==> padwave"
"$CXX" -std=c++23 -Wno-c23-extensions -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra \
  -I. $CFLAGS -o padwave padwave.cc $LIBS
