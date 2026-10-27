# padwave — handoff for the next session

## State (verified)

- Repo: this directory; deployed at `ebrahim@192.168.1.175:~/padwave`.
- HEAD: `e7d1c78 input frames are a struct callback now`.
- Build: `./build.sh` (generates `padwave_ui.hh` from `index.html`, compiles and
  runs `test_wire.cc`, then builds `./padwave`). `./install.sh` builds,
  installs to `~/.local/bin/padwave`, installs the user unit and restarts it.
- Service: systemd user unit `padwave.service`, one process / one thread, one
  listener on 8787, ~2 % CPU, radio playing, controller connected.
  UI: `http://192.168.1.175:8787`.
- Layout: `padwave.cc` (one TU), `padwave_wire.hh` (the only wire description),
  `index.html` (panel), `test_wire.cc`, `build.sh`, `install.sh`,
  `padwave.service`, `padwave-boot.sh`, `padwave-ctl`, `deploy.sh`, `docs/`.

Passes already landed:

- `fbabaec` — player status/track struct callbacks; player text/sockets gone.
- `b003f78` — `/api/front` served from the app, not the status socket.
- `aecd18a` — dead status socket/parser/datagram/`padwave_status` removed.
- `e7d1c78` — input frames are a struct callback; frame parser deleted.

## Preferences / constraints

- One TU at the root; no STL, no RTTI, no inheritance; C++ methods for state
  (the app is a `struct app` with methods; the server/app boundary is opaque).
- Callbacks take userdata; avoid file-scope state. (The input's decoder state is
  still file-scope and is part of the next pass.)
- No JSON requirement: line- or key=value formats are fine, even for the panel;
  adapt `index.html`'s small consumer if you change `/api/status`,
  `/api/events`, `/api/front` or `/api/command`.
- No extra CLI apps except what makes sense for testing.
- The wire test is fast and runs on every `build.sh`; keep it green.
- Edit in small blocks and build on the Pi after each. Avoid blind global
  replacements: past failures were an over-broad `g.` replace (corrupted
  `stdarg.h`), an ambiguous `app` (struct inside an anonymous namespace vs. the
  header forward declaration), and duplicated call arguments. The player and
  input passes worked when anchored on exact snippets and built after each
  block.

## Remaining pass 1 — input actions/events/mic + decoder state

The input still formats action/event/mic text and the app still parses it:

- `g_input_sink` / `input_emit` (~1246–1290)
- `track_dpad`, `track_buttons`, `track_triggers`, `track_sensors`
- `emit_action` / `emit_event` (~1543/1548); call sites include
  `emit_action("seek -6")`, volume ±, `next_random`, `previous`, `suspend`,
  `jack_plugged/unplugged`; events `activity`, `shake`, `trigger l/r ...`.
- `handle_report(input_frame_fn, void *ud, …)` (~1641)
- `input_set_sink`, `input_string`/`input_feed`, `input_main` (~1655–1760)
- App: `input_sink` / `input_line` (~2122/2329), the text branch; the frame
  branch is already deleted.

Plan:

1. Move `struct input_handle` above the input functions and give it the four
   callbacks (`frame`, `action`, `event`, `mic`) + `ud`, plus the decoder state
   (`g_dpad`, `g_actions`, jack/shake/trigger state).
2. Thread `input_handle *` (or a small `input_ctx`) through `handle_report`,
   `track_*`, `emit_action`, `emit_event`; change action strings to
   `(name, value)` and events to `(name)`; mic to `(data, len)`.
3. Add `input_action_cb` / `input_event_cb` / `input_mic_cb` to `app` (static
   methods, `this` as ud); map actions to `next/previous/shuffle_game/
   toggle_pause/set_volume/seek/…`; delete `input_sink`, `input_line` and the
   text sink argument from `input_set_sink`.
4. Build, install, verify pause/next/volume/seek from the controller and that
   `/api/status` shows controller state.

## Remaining pass 2 — server context + dead proxy + format

- Server globals: `g_conns`, `g_owner`, `g_epoll`, `g_backend_port`,
  `g_standalone`, `g_timer_fd`, `g_start_ms`, `g_requests`, `g_index_response`,
  `g_index_len`. Replace with a `struct server` passed to `close_conn`,
  `add_fd`, `mod_fd`, `read_client`, `flush_client`, `append_*`, the route
  helpers and `server_main`; keep the app pointer in it.
- Delete the dead proxy: `g_backend_port`, `build_upstream_request`,
  `start_proxy`, `write_upstream`, `read_upstream`, the `conn.up*` fields and
  the `/api/` proxy branch. The app is the only backend.
- Optional (allowed): replace the status/front/command JSON with line/key=value
  data and adapt `index.html`; this removes `json_escape`/`json_number`/
  `json_string` and the JSON builders. Keep `/api/library` and `/api/config`
  shapes whatever is simplest for the panel.

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
   curl -s http://127.0.0.1:8787/api/status | head -c 300; \
   curl -s http://127.0.0.1:8787/api/front  | head -c 220'
```

`./deploy.sh` does the rsync + install if `PADWAVE_HOST` is set.
