# padwave — handoff for the next session

## State (verified)

- Repo: this directory; deployed at `ebrahim@192.168.1.175:~/padwave`.
- HEAD: `45f5795 server state is a struct, the proxy is gone, and the hot
  routes are key=value`.
- Build: `./build.sh` (generates `padwave_ui.hh` from `index.html`, compiles and
  runs `test_wire.cc`, then builds `./padwave`). `./install.sh` builds,
  installs to `~/.local/bin/padwave`, installs the user unit and restarts it.
- Service: systemd user unit `padwave.service`, one process / one thread, one
  listener on 8787, ~1.7 % CPU, radio playing, controller connected.
  UI: `http://192.168.1.175:8787`.
- Layout: `padwave.cc` (one TU), `padwave_wire.hh` (the only wire description),
  `index.html` (panel), `test_wire.cc`, `build.sh`, `install.sh`,
  `padwave.service`, `padwave-boot.sh`, `padwave-ctl`, `deploy.sh`, `docs/`.

Passes already landed:

- `fbabaec` — player status/track struct callbacks; player text/sockets gone.
- `b003f78` — `/api/front` served from the app, not the status socket.
- `aecd18a` — dead status socket/parser/datagram/`padwave_status` removed.
- `e7d1c78` — input frames are a struct callback; frame parser deleted.
- `984fce3` — input actions/events/mic are callbacks; the decoder's edge state
  (`actions`, `sensors`, `dpad`) lives in `input_handle`; the text sink,
  `input_emit` and the `g_*` input globals are gone. The app installs
  `input_action_cb` / `input_event_cb` / `input_mic_cb` with `this` as ud.
- `45f5795` — one `struct server` owns the connection table, fd->conn map,
  epoll/timer fds, counters, embedded index and app pointer; the dead proxy
  (`g_backend_port`, upstream request/relay, `conn.up*`, `/api/*` fallback) is
  gone; `/api/status`, `/api/front`, `/api/command` and both SSE streams speak
  dotted `key=value`; `/api/library`, `/api/config`, `/api/schema` stay JSON.

## Preferences / constraints

- One TU at the root; no STL, no RTTI, no inheritance; C++ methods for state.
- No C++ `new`/`delete`: the app is a stack object in `server_main`, the large
  `server` table is a zero-initialized function-local static. Keep it that way.
- Callbacks take userdata; no file-scope state. Input state is in
  `input_handle`; server state is in `struct server` passed to every helper.
- `padwave serve [port]`; the in-process app is the only backend. No proxy.
- Hot routes are `key=value` lines: dotted keys nest, `true`/`false`/`null`
  are literals, comma lists for `accel`/`gyro`/`buttons`. SSE prefixes every
  line with `data:`. `index.html` has `parseKV`; keep the two sides in sync.
- The wire test is fast and runs on every `build.sh`; keep it green.
- Edit in small blocks and build on the Pi after each. Avoid blind global
  replacements: past failures were an over-broad `g.` replace, an ambiguous
  `app`, and duplicated call arguments.
- The service is running at the HEAD above; don't leave it broken. The old
  e7d1c78 binary is backed up on the Pi as `~/padwave.e7d1c78.bak`.

## What is genuinely missing

1. The lab endpoints are still `not implemented`: `/api/output` (lightbar,
   player LEDs, rumble, trigger effects, raw fields, mic monitor),
   `/api/mic/reset`, `/api/debug/trigger`, `/api/reload`. The panel renders
   them but they only produce the not-implemented JSON toast. Either wire them
   to the player's output path or delete those panel sections.
2. Idle fade/suspend and controller power-off, jack-pause, persisted
   config/playhead, and mic level decoding were never ported from the Elixir
   app. `session.*`, `audio.*`, `hid.*`, `mic.*` in `status_text` are static.
3. The status stream omits `controller.fields` and `controller.touches`, so
   the panel shows `report seq —`, `rate —/s` and an empty touchpad. The frame
   struct has `seq`, `tfl`, `tfr`; touches are not carried yet.
4. `/api/config` is static: POST returns `ok=true` but ignores the body. The
   panel's trigger/idle controls do not persist.
5. Optional cleanup: convert `/api/library`/`/api/config`/`/api/schema` to
   text too and delete `json_escape`/the JSON builders, if it is worth the
   list encoding. Right now only the hot routes are text.
6. `status_text` calls `find_dualsense()` every 500 ms just for the node name;
   caching it would save the sysfs scan.

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
   curl -s -X POST --data-binary "action=pause" http://127.0.0.1:8787/api/command; \
   curl -s -X POST --data-binary "action=pause" http://127.0.0.1:8787/api/command; \
   curl -sN --max-time 2 http://127.0.0.1:8787/api/events | head -8'
```

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
(remember `--no-proxy-server` for the LAN URL); a paused/resumed click and the
front/status SSE were verified that way.
