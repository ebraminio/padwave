# fuzzers

`fuzz_input` feeds arbitrary bytes to the controller input engine, the same
path the hidraw fd drives. It needs only clang/compiler-rt:

    ./fuzz/build.sh
    ./fuzz/fuzz_input fuzz/corpus -max_len=512 -max_total_time=60

The corpus has a 0x31 Bluetooth report, a 0x01 USB report, a mic frame and a
couple of short inputs. Run with `-rss_limit_mb=...` on small machines.

Next targets: `parse_request` (after separating it from the epoll read) and the
field/command text parsers.
