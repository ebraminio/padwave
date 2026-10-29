#!/usr/bin/env bash
# Build the fuzzers. Needs clang/compiler-rt (libFuzzer) and libopus;
# no FFmpeg.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-clang++}"
LIBS="-lopus -lm"
"$CXX" -std=c++23 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined \
  -I. fuzz/fuzz_input.cc -o fuzz/fuzz_input $LIBS

"$CXX" -std=c++23 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined \
  -I. fuzz/fuzz_input_seq.cc -o fuzz/fuzz_input_seq $LIBS

"$CXX" -std=c++23 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined \
  -I. fuzz/fuzz_fields.cc -o fuzz/fuzz_fields $LIBS

"$CXX" -std=c++23 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined \
  -I. fuzz/fuzz_http.cc -o fuzz/fuzz_http $LIBS
