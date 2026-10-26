#!/bin/sh
# Builds and runs the C++ wire-layout test. No STL, no RTTI, no exceptions.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

c++ -std=c++23 -fno-exceptions -fno-rtti -O2 -I "$here" \
  -o /tmp/padwave_test_wire "$here/test_wire.cc" -lz

exec /tmp/padwave_test_wire
