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

## LED / playback status system (reference for the port)

The old design had **one HID writer** and a **state-coloured lightbar**:

- `Output.Hid` owns the 0x31 field map, seq/tag/CRC and the persistent
  `power_save` claim; every feature (lightbar, player LEDs, triggers, rumble,
  mute) sends a `fields ...` line through it. The native player owns the same
  `OutputBody` and embeds that snapshot in **every 0x36 audio report**, so the
  LEDs/triggers/mute survive while audio flows; a 0x31 report is used when
  there is no audio.
- Playback state → lightbar (`Output.Lightbar` subscribes to the Bus):
  playing `{0,140,255}` cyan; paused `{255,110,0}` amber; muted
  `{120,60,255}` purple; sleep-timer warning red `{255,0,0}` pulsing every
  600 ms at scale 255/60; suspended/off `{0,0,0}`. A custom colour overrides
  until the next state change; device-down goes dark; device-up keeps the
  custom colour and forces one repaint (`painted: nil`) because the kernel
  startup animation resets the bar. Unchanged colours are not written
  (volume events are frequent).
- Charging (`Output.ChargingLeds`, fed by controller frames): `charging`/`full`
  sweeps the five player LEDs between `0b01010` and `0b10101` every 3 s
  (`charging_leds_ms`), and returns to the kernel's player-1 pattern
  `0b00100` on unplug/device-down.
- Fields/claims: `valid_flag1` bit 1 `power_save`, bit 2 `lightbar`, bit 3
  `reset_lights`, bit 4 `player_leds`; `valid_flag2` bit 0 brightness, bit 1
  colour fade / `lightbar_setup` (`0x02` = fade out); `power_save_control`
  bits 4/6 mic/audio mute. The old `Report` field maps and the lab's
  lightbar/player-LED/power-save/trigger/rumble rows used exactly these.
- Status sources: `Player` (pos/dur/sent/underruns/playing/paused),
  `Radio` (transport, volume/mute, current, idle_warning, suspended,
  resume_pending), `Session` (phase, idle_seconds, warning_in, shutdown_in),
  `Hid.stats` (writes/errors/recent/power_save/player_leds), `Lightbar.status`
  and the controller frame. They were published on the Bus and in
  `/api/status`/SSE; the panel's idle chip, warning bar and lab mirrored them.
- Current C++ state: the player still has `OutputBody` and the `fields`
  command, but the app does not drive the lightbar/player LEDs/charging/triggers
  (`/api/output` returns 501, `led`/`hid` in `/api/status` are static). Port
  the old `lightbar.ex`/`charging_leds.ex` as app methods, call
  `player_command("fields ...")` and feed the charging state from the input
  frame.
