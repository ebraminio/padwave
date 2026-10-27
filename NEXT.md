# padwave — handoff

## State

- Repo: this directory; deployed at `ebrahim@192.168.1.175:~/padwave`.
- HEAD: latest `main`; the behavior described here landed through `2cda98a`
  (later commits are documentation only). `git log --oneline` for detail.
- Build: `./build.sh` (UI header, wire test, binary). `./install.sh` builds,
  installs `~/.local/bin/padwave`, installs the user unit and restarts it.
- Service: `padwave.service`, one process / one thread, port 8787, ~2 % CPU,
  controller connected, UI at `http://192.168.1.175:8787`.
- Layout:
  - `padwave.cc` — engines (player, input, opusdec) + main
  - `padwave_app.hh` — `struct App` (private fields, inline camelCase methods)
  - `padwave_server.hh` — `struct Server` (epoll, routes, SSE, embedded UI)
  - `padwave_wire.hh` — the only wire description
  - `index.html` — panel, embedded at build time
  - `.clang-format`, `test_wire.cc`, `build.sh`, `install.sh`, `deploy.sh`
- Config lives in `~/.config/padwave/config`: idle timeout/grace, poweroff
  delay, trigger effect values, `play_path`/`play_pos_ms`.
- Recent work: server/lab text routes, App/Server class split, idle
  fade/suspend/wake, BlueZ power-off + hidraw reopen, playhead persistence,
  jack pause, Opus mic level/monitor, volume-zero == pause.

## Constraints

- One translation unit at the root. No STL, RTTI, inheritance, or C++
  `new`/`delete`. The App is a stack object in `server_main`; the Server table
  is a function-local static.
- `struct App` / `struct Server`: private fields, public surface, inline
  camelCase methods. Free functions and wire names stay snake_case.
- Run `clang-format -i padwave.cc padwave_app.hh padwave_server.hh
  padwave_wire.hh test_wire.cc` before committing.
- Callbacks take userdata. Input state is in `input_handle`; server state is in
  `Server`; no file-scope state.
- All routes are key=value (dotted keys, `true`/`false`/`null`, comma lists for
  `accel`/`gyro`/`buttons`). SSE prefixes each line with `data:`.
  `/api/library` is TSV. `index.html` has `parseKV`; keep both sides in sync.
- Volume 0 is the paused state. `muted` is derived; resume/mute/new controller
  restores the last nonzero volume; default is 15. Do not add a separate mute
  flag. `setVolume`, `togglePause`, `ensureAudibleVolume` and `playerStart`
  carry this model.
- Power-off is a BlueZ disconnect after idle (`bluetoothctl disconnect
  <HID_UNIQ>`, `timeout 5`), not a 0x31 bit. The pad sleeps; press PS to wake.
  The player reopens its hidraw on write failure/missing node.
- Build on the Pi after each block; keep the wire test green. Don't leave the
  running service broken.

## Remaining / known gaps

1. `audio.present`, `audio.sink`, `audio.state` are static; the panel always
   says "no sink". The player does not report the ALSA sink.
2. Idle fade floor is fixed at 10%; only timeout/grace/poweroff delay are
   configurable. Mic monitor state is not persisted (off after restart).
3. Playhead is saved 1 s after changes and every 30 s while playing, so a hard
   crash can lose up to 30 s (shutdown/power-off save exactly). Volume is not
   persisted.
4. Jack pause and the lab effects have API/decoder verification only; physical
   unplug/lightbar/rumble/trigger response has not been observed.
5. If the controller mic LED blinks while the monitor switch is on, that is the
   firmware's capture indicator; the app only forces `power_save`/mic volume.
   A stable `MuteLed` claim can be added if needed.
6. `/api/reload` is a deliberate no-op (`reloaded=0`); there are no modules.

## Verify

```sh
rsync -az --delete --exclude '.git' --exclude 'padwave' \
  --exclude 'padwave_ui.hh' --exclude 'var' ./ ebrahim@192.168.1.175:~/padwave/
ssh ebrahim@192.168.1.175 'cd ~/padwave && ./build.sh && ./install.sh'

ssh ebrahim@192.168.1.175 \
  'systemctl --user is-active padwave; \
   ps -o pid,nlwp,%cpu,args -p $(pgrep -x padwave); \
   curl -s http://127.0.0.1:8787/api/status | head -15; \
   curl -s http://127.0.0.1:8787/api/front; \
   curl -s http://127.0.0.1:8787/api/config'
```

Quick checks:

```sh
# volume zero pauses; resume restores a nonzero volume
curl -s -X POST --data-binary $'action=volume\nvalue=0' \
  http://127.0.0.1:8787/api/command
curl -s http://127.0.0.1:8787/api/status | grep -E 'radio.(volume|paused|muted)'
curl -s -X POST --data-binary 'action=pause' http://127.0.0.1:8787/api/command

# mic (must be two lines)
curl -s -X POST --data-binary $'kind=mic_monitor\nenabled=true' \
  http://127.0.0.1:8787/api/output
sleep 2; curl -s http://127.0.0.1:8787/api/status | grep '^mic\.'
curl -s -X POST --data-binary $'kind=mic_monitor\nenabled=false' \
  http://127.0.0.1:8787/api/output
```

For idle, set `idle_timeout_ms=3000`, `idle_warn_grace_ms=3000` via
`POST /api/config`, watch `session.phase`, wake with a command, then restore
`900000`/`10000`. `poweroff_when_idle_ms=0` disables the BlueZ disconnect while
testing. A service restart should resume the saved track/position.

## Testing without pressing buttons

- `./padwave input --hex <128 hex>` decodes a USB 0x01 report (64 bytes: id +
  63-byte body; d-pad bits 0-3, 8 = centre). No callbacks means actions/events
  print as lines.
- A FIFO of reports exercises the stateful decoder (d-pad hold ramp, trigger
  volume, jack debounce).
- The panel can be driven headlessly with Chrome DevTools Protocol; use
  `--no-proxy-server` for the LAN URL.
