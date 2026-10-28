# fuzzers

`fuzz_input` feeds arbitrary bytes to the controller input engine, the same
path the hidraw fd drives. It needs only clang/compiler-rt:

    ./fuzz/build.sh
    ./fuzz/fuzz_input fuzz/corpus -max_len=512 -max_total_time=60

The corpus has a signed 0x31 Bluetooth report, a 0x01 USB report, a mic frame
and a couple of short inputs.

`fuzz_input_seq` feeds a sequence of 78-byte 0x31 reports through one `Input`
so the d-pad/PS/jack edge state carries across reports; the mutator re-signs
each 0x31 block. Seeds are in `corpus_seq/`.

`fuzz_fields` feeds arbitrary text to `padwave_fields::apply_fields()`; seeds
are in `corpus_fields/`.

`fuzz_http` feeds arbitrary bytes to `HttpRequest(buf, len)` (padwave_http.hh),
the pure request-head parser the server uses. Seeds are in `corpus_http/`.

    ./fuzz/build.sh
    ./fuzz/fuzz_http fuzz/corpus_http -max_len=16384 -max_total_time=60

Run with `CXX=/opt/homebrew/opt/llvm/bin/clang++` on macOS (Xcode's clang has
no libFuzzer); the Pi's clang 19 works as-is. Use a scratch corpus copy if you
do not want libFuzzer writing new units into the repo:

    cp -r fuzz/corpus_http /tmp/http_corpus
    ./fuzz/fuzz_http /tmp/http_corpus -max_len=16384 -max_total_time=60

All four run in `.github/workflows/ci.yml`'s `fuzz-smoke` job for 20 s each,
with a scratch corpus per target. The build job uses clang 19 for `#embed`.
