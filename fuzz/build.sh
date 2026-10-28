#!/usr/bin/env bash
# Build the input fuzzer. Needs clang/compiler-rt (libFuzzer), no FFmpeg/opus.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined \
  -I. fuzz/fuzz_input.cc -o fuzz/fuzz_input

"$CXX" -std=c++23 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined \
  -I. fuzz/fuzz_input_seq.cc -o fuzz/fuzz_input_seq

"$CXX" -std=c++23 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined \
  -I. fuzz/fuzz_fields.cc -o fuzz/fuzz_fields

"$CXX" -std=c++23 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined \
  -I. fuzz/fuzz_http.cc -o fuzz/fuzz_http
