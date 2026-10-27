#!/usr/bin/env bash
# Build padwave: one source, one binary. The wire test is fast, so it is
# compiled and run on every build.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-c++}"
PKGS="libavformat libavcodec libswresample libavutil"
CFLAGS="$(pkg-config --cflags $PKGS)"
LIBS="$(pkg-config --libs $PKGS) -lopus -lz -lm"

echo "==> ui header"
python3 - > padwave_ui.hh <<'PY'
data = open("index.html", "rb").read()
print("#pragma once")
print("#include <stddef.h>")
print("static const unsigned char kIndexHtml[] = {")
print(",".join(str(b) for b in data))
print("};")
print("static const size_t kIndexHtmlSize = sizeof(kIndexHtml);")
PY

echo "==> wire test"
"$CXX" -std=c++23 -O2 -fno-exceptions -fno-rtti -I. -o /tmp/padwave_test_wire test_wire.cc -lz
/tmp/padwave_test_wire

echo "==> padwave"
"$CXX" -std=c++23 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra \
  -I. $CFLAGS -o padwave padwave.cc $LIBS
