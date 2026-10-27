# padwave — handoff for the next session

## State (verified)

- Repo: this directory; deployed at `ebrahim@192.168.1.175:~/padwave`.
- HEAD: `2e4b84d add an LLVM-based .clang-format and format the C++
  sources`.
- Build: `./build.sh` (generates `padwave_ui.hh` from `index.html`, compiles and
  runs `test_wire.cc`, then builds `./padwave`). `./install.sh` builds,
  installs to `~/.local/bin/padwave`, installs the user unit and restarts it.
- Service: systemd user unit `padwave.service`, one process / one thread, one
  listener on 8787, ~2 % CPU, radio playing, controller connected.
  UI: `http://192.168.1.175:8787`.
- Layout: `padwave.cc` (engines + main), `padwave_app.hh` (struct App),
  `padwave_server.hh` (struct Server), `padwave_wire.hh` (the only wire
  description), `index.html` (panel), `.clang-format` (LLVM, 100 cols),
  `test_wire.cc`, `build.sh`, `install.sh`, `padwave.service`,
  `padwave-boot.sh`, `padwave-ctl`, `deploy.sh`, `docs/`.

Passes already landed:

- `fbabaec` — player status/track struct callbacks; player text/sockets gone.
- `b003f78` — `/api/front` served from the app, not the status socket.
- `aecd18a` — dead status socket/parser/datagram/`padwave_status` removed.
- `e7d1c78` — input frames are a struct callback; frame parser deleted.
- `984fce3` — input actions/events/mic are callbacks; decoder state lives in
  `input_handle`; the text sink and input globals are gone.
- `45f5795` — one `struct server` owns the connection table, epoll/timer fds,
  counters, embedded index and app pointer; the dead proxy is gone; the hot
  routes became key=value.
- `52b32c0` — touches/seq/trigger feedback/rate reach the app and status; the
  hidraw node is cached; config loads/saves `~/.config/padwave/config` and is
  served as key=value; the panel posts text.
- `f09cafb` — the lab works through the player's `fields` reports (lightbar,
  player LEDs, power save, rumble, trigger effects, raw maps; timed stops);
  `/api/debug/trigger`, `/api/mic/reset`, `/api/reload` answer; library is TSV
  and schema is `bindings.*`, removing the last JSON builders.
- `246a3ae` — App and every method body moved to `padwave_app.hh`; fields are
  private, methods are inline camelCase, and the server calls App directly.
- `39dd81c` — `/health` and the unknown-API fallback are text too, so no route
  emits JSON.
- `b45a5c3` — struct Server moved to `padwave_server.hh`; private nested Conn
  table, private fields, inline camelCase helpers, public run(); server_main is
  the static-Server wrapper.
- `2e4b84d` — `.clang-format` (LLVM/100 cols/2-space, private fields, compact
  guards) and the whole C++ tree formatted with it.

## Preferences / constraints

- One TU at the root; no STL, no RTTI, no inheritance.
- No C++ `new`/`delete`: the app is a stack object in `server_main`, the large
  `server` table is a zero-initialized function-local static. Keep it that way.
- `padwave_app.hh` is the class: `struct App`, inline method definitions,
  private fields, Java-style camelCase methods. Free functions and the
  wire/protocol names keep snake_case.
- Format: `.clang-format` at the root (LLVM, 100 columns, 2-space, access
  modifiers outdented, includes unsorted). Run `clang-format -i` on the five
  C++ files before committing.
- Callbacks take userdata; no file-scope state. Input state is in
  `input_handle`; server state is in `struct Server` (`padwave_server.hh`)
  passed through its methods.
- `padwave serve [port]`; the in-process app is the only backend. No proxy.
- Every route is key=value (dotted keys nest, `true`/`false`/`null` literals,
  comma lists for `accel`/`gyro`/`buttons`). SSE prefixes every line with
  `data:`. `/api/library` is tab-separated. `index.html` has `parseKV`; keep
  the two sides in sync.
- The wire test is fast and runs on every `build.sh`; keep it green.
- Edit in small blocks and build on the Pi after each. Avoid blind global
  replacements: past failures were an over-broad `g.` replace, an ambiguous
  `app`, and duplicated call arguments.
- The service is running at the HEAD above; don't leave it broken. The old
  e7d1c78 binary is backed up on the Pi as `~/padwave.e7d1c78.bak`.

## What is genuinely missing

1. Idle is not implemented. `idle_timeout_ms`/`idle_warn_grace_ms` are saved
   and shown, but nothing expires; `session.phase`, `warning_in`,
   `shutdown_in` and `fade_percent` in `status_text` are static. There is no
   idle fade, suspend, power-off or resume-on-activity.
2. Jack pause is not implemented. `jack_plugged`/`jack_unplugged` reach
   `handleAction` but fall through to only recording `last_action`; there is no
   `paused_by_jack` behaviour.
3. Playhead is not persisted. Startup picks a random station; only config is
   saved. A small `path`/`position` in the config file plus a resume on init
   would finish this.
4. Mic level decoding is not implemented. `input_mic_cb` discards the Opus
   frame, the `mic.*` status fields are static, and the monitor switch returns
   `ok=false error=mic monitor is not implemented`.
5. `/api/reload` is a deliberate no-op (`reloaded=0`): there are no modules to
   reload in a single binary.
6. The lab writes real reports (`ok 1` in the journal) but the physical
   lightbar/rumble/trigger response was not observed; only the API and the
   `fields` writes were verified. Timed stops run from the 10 ms player timer.

## Verify commands

```sh
# from this directory
rsync -az --delete --exclude '.git' --exclude 'padwave' \
  --exclude 'padwave_ui.hh' --exclude 'var' ./ ebrahim@192.168.1.175:~/padwave/
ssh ebrahim@192.168.1.175 'cd ~/padwave && ./build.sh && ./install.sh'

ssh ebrahim@192.168.1.175 \
  'systemctl --user is-active padwave; \
   ps -o pid,nlwp,%cpu,args -p $(pgrep -x padwave); \
   ss -ltnp | grep 8787; \
   curl -s http://127.0.0.1:8787/api/status | head -12; \
   curl -s http://127.0.0.1:8787/api/front; \
   curl -s http://127.0.0.1:8787/api/config; \
   curl -s http://127.0.0.1:8787/api/library | head -2; \
   curl -s http://127.0.0.1:8787/api/schema | head -3; \
   curl -s -X POST --data-binary "kind=lightbar;rgb=0,160,255" \
     http://127.0.0.1:8787/api/output; \
   curl -s -X POST --data-binary "action=pause" http://127.0.0.1:8787/api/command; \
   curl -s -X POST --data-binary "action=pause" http://127.0.0.1:8787/api/command'
```

(The lightbar example must be two lines: `kind=lightbar` newline `rgb=...`.)

`./deploy.sh` does the rsync + install if `PADWAVE_HOST` is set.

## Decoder checks without pressing a button

USB 0x01 reports (64 bytes: id, then the 63-byte body; dpad in bits 0-3, 8 =
centred) go through the CLI with no callbacks, so actions/events print as
lines:

```sh
./padwave input --hex <128 hex chars>
```

A FIFO of successive reports exercises the stateful parts (d-pad hold ramps
`seek 6/10/20/40/60/60`, R2 ramps `volume`, trigger press/release events, jack
debounce). The panel can be checked headlessly with Chrome's DevTools protocol
(remember `--no-proxy-server` for the LAN URL); status/config/library/schema,
the lab kinds and a pause click were verified that way.
