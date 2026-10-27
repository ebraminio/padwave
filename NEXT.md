# padwave — handoff

## State

- Repo: this directory; deployed at `ebrahim@192.168.1.175:~/padwave`.
- HEAD: latest `main`; the behavior described here landed through `01f685c`
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
  jack pause, Opus mic level/monitor, volume-zero == pause, state-coloured
  lightbar + charging player LEDs.

## Constraints

- One translation unit at the root. No STL, RTTI, inheritance, or C++
  `new`/`delete`. The App is a stack object in `server_main`; the Server table
  is a function-local static.
- `struct App` / `struct Server`: private fields, public surface, inline
  camelCase methods. App-only helpers are private static members; Server
  constants and its bindings text are private. The only remaining free
  interface is the shared engine API (player/input/main/find_dualsense), which
  genuinely belongs to padwave.cc. Free functions and wire names stay
  snake_case.
- Run `clang-format -i padwave.cc padwave_app.hh padwave_server.hh
  padwave_wire.hh test_wire.cc` before committing.
- CRC-32 is local in `padwave_wire.hh` (standard reflected polynomial); do not
  add `-lz` back. FFmpeg's shared libraries may still bring libz in
  transitively.
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
7. `hid.power_save` is still null; the app does not expose the last
   `power_save_control` byte even though the mic path claims it.

## Verify

```sh
# from this directory: sync + build + install + restart on the Pi
PADWAVE_HOST=ebrahim@192.168.1.175 ./deploy.sh

# or manually
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

## LED / playback status system

This is now implemented in `App` (`f4de004`). The player remains the one HID
writer: `App` sends `fields ...` through `player_command`, the player merges the
`OutputBody` into its persistent state and embeds it in every 0x36 audio report;
with no audio the same `fields` line produces a 0x31 report.

- Lightbar state: playing cyan `{0,140,255}`, paused amber `{255,110,0}`,
  muted purple `{120,60,255}`, idle warning red alternating 255/60 every
  600 ms, suspended/off/device-down black. A lab custom colour overrides the
  state until the next playback/session state change, survives device-down/up,
  and device-up forces one repaint. Reset drops the custom colour and repaints
  the state colour.
- Player LEDs: `charging`/`full` sweeps `0b01010`/`0b10101` every 3 s; otherwise
  the kernel player-1 pattern `0b00100`, or a lab manual mask while not
  charging. Device-down returns to `0b00100`.
- Only changed values are written. `serviceLeds()` runs from the 10 ms player
  timer and the app tick; player start/reconnect/shutdown force a repaint
  (`now_ms` pulse, charging transitions and state changes compare before send).
- Status: `led.mode` (`off/playing/paused/muted/warning/custom`),
  `led.red/green/blue`, `led.custom`, `led.painted`, and the real
  `hid.player_leds` mask (the panel's five dots follow it).
- Claims: `valid_flag1` `lightbar`/`player_leds` (and the player's persistent
  `power_save` claim from the mic path); `valid_flag2` brightness/fade are
  still one-shot lab commands. The app no longer merges `reset_lights` or
  `color_fade` into the 0x36 state.
- Physical LED appearance is not verified; status reflects what was written.
