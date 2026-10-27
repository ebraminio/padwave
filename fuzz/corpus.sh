#!/usr/bin/env bash
# Rebuild the input seed corpus from the wire layout.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -I. fuzz/make_corpus.cc -o /tmp/padwave_make_corpus
/tmp/padwave_make_corpus fuzz/corpus
