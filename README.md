# padwave

A DualSense-controlled music player for Linux. One controller over
Bluetooth, a folder of audio files, and the controller's headphone jack as the
speaker.

A track is a file. Albums, long mixes and downloaded podcasts already work;
folders are scanned recursively.

```
DualSense ──BT──▶ hidraw ──▶ decode ──▶ controls ──▶ player ──▶ 0x36
   ▲                                                              │
   └──────────── lightbar, rumble, triggers (0x31) ◀──────────────┘
```

One binary, one process, one thread: `padwave [port]` runs the player, input
decoder, panel and HTTP API from the same epoll loop.

## Controls

| Input | Action |
|---|---|
| L2 / R2 | volume down / up — deeper press, faster change |
| d-pad ← / → | seek −6 s / +6 s; hold to steer, ramping 10 → 20 → 40 → 60 s |
| d-pad ↑ / ↓ | next / previous track |
| △ | pause / resume |
| ○ | play (stands in for this controller's dead ✕) |
| □ | shuffle another folder |
| L1 / R1, Create / Options, L3 / R3 | previous / next track |
| touchpad | toggle power-save mode |
| mute | mute / unmute |
| PS held ~2 s | save the playhead and power the controller off |
| Edge paddles, Fn buttons | previous / next, pause, mute |

Analogue sticks are deliberately unbound: ours are worn and jitter.

## Build and run

Needs a C++ compiler, `pkg-config`, FFmpeg (`libavformat`, `libavcodec`,
`libswresample`, `libavutil`, `libavfilter`), `libopus` and `python3` for the embedded UI.

```sh
./build.sh
PADWAVE_ROOTS=/path/to/audio ./padwave [port]
# panel: http://<host>:8787
```

`PADWAVE_ROOTS` is a colon-separated list scanned recursively; without it the
app uses `~/Music`. `PADWAVE_HOST`/`PADWAVE_DIR` configure `deploy.sh`. The wire
layout, field-offset and CRC assertions compile as part of `dualsense.hh`.
Public packet types are `InputReport`, `OutputReport`, `UsbOutputReport`,
`AudioReport`, `MicFrame`, and the composable input/output bodies
(`InputBody`, `OutputBody`). The endian wrappers (`NumType`), feature payloads
and test probes live in `dualsense::internal`.
The build uses `-Weverything` with a short suppression list and reports the
compile's real/user/sys time.

## Install

On the host, from the project root:

```sh
./install.sh
```

This builds, installs `~/.local/bin/padwave`, installs the systemd user unit and
restarts it. The panel is LAN-reachable on port 8787 with no authentication;
keep it on a trusted network.

## Deploy

From the development machine, sync source (not the local binary) and rebuild on
the host:

```sh
PADWAVE_HOST=user@host ./deploy.sh
```

The manual equivalent:

```sh
rsync -az --delete --exclude '.git' --exclude 'padwave' \
  --exclude 'var' ./ user@host:~/padwave/
ssh user@host 'cd ~/padwave && ./build.sh && ./install.sh'
```

## HTTP

Routes speak `key=value` lines; dotted keys nest and `true`/`false`/`null` are
literals. `GET /api/status`, `/api/events` (SSE), `/api/front` and
`/api/front/events` are the live state. `POST /api/command` accepts `next`,
`previous`, `pause`, `mute`, `shuffle_folder`, `disconnect`, `volume`, `speed`, `seek`,
`seek_to` and `play_track` with the corresponding value/delta/position/id.
`GET /api/library` is TSV; `/api/config` persists settings; `/api/schema`
describes the lab fields; `POST /api/output` is the controller lab.

## Behavior

- **Playback:** track scan, shuffle, history, gapless transitions, crossfade,
  natural numeric order (ep2 before ep10), resume of the saved track/position,
  and volume/mute. Next/previous and gapless track end follow that sorted list;
  the shuffle button is the only random action. Speed is 0.75×–2× via FFmpeg's
  pitch-preserving `atempo`, persisted across restarts.
- **Volume:** zero is the paused state. Resume restores the last non-zero
  volume; 15% is used only when no non-zero value has ever been stored. Current
  and last non-zero volumes persist across restarts.
- **Shared USB host:** when a USB data host is attached over Bluetooth, the
  controller's buttons/triggers/PS bindings and all lightbar writes are
  suppressed so they are not applied twice; input state, audio and jack routing
  continue.
- **Microphone:** monitoring is independent of playback. Enabling it requests
  microphone capture and reports a live PCM level; playback can continue while
  monitoring. The mute-button LED is claimed once on each pause transition:
  lit while paused, off while playing. Mic on/off reasserts the current pause
  state with a single 0x31, but 0x36 never carries mute_led.
- **Lightbar:** cyan playing, amber paused, purple muted, red pulse on idle
  warning, black suspended/off. While charging, setup 2 runs for 2 s, then
  setup 1 for 2 s, repeating with no RGB frames; full is solid white. The lab
  can set a colour or use `lightbar_setup` 1/2 to turn the bar on/off.
- **Output route:** headphone jack when present. Pulling it pauses; plugging it
  switches an already-playing stream back without pausing. With no jack, a
  device power-on waits for an explicit play, and that explicit play/resume
  uses the controller speaker. Works over USB and Bluetooth.
- **Player LEDs:** left-to-right battery bar (45% → `11100`, full → `11111`);
  at 15% or below the first LED blinks `10000`/`00000` every 3 s while
  discharging. Charging is a solid bar.
- **Idle:** fades the volume over the grace, suspends playback, and after
  `poweroff_when_idle_ms` asks BlueZ to disconnect the pad. Press PS to wake.
  Without a controller, playback pauses and the player tick slows to 500 ms.

## Bluetooth HID tuning

If `bluetoothd` sits at a few percent CPU while the controller is idle, it is
likely relaying HID in userspace. Set `UserspaceHID=false` in
`/etc/bluetooth/input.conf`, load `hidp`, and restart `bluetooth.service`.
Kernel HIDP keeps the 500 Hz report path out of bluetoothd (~3% → ~0.1%).

## Formatting

C++ sources use `.clang-format`: LLVM, 100 columns, 2-space indent, a blank line
between definitions, private class fields and compact guards. Run clang-format
19.1.7 over `padwave.cc` and the headers, including `padwave_app_stub.hh`.

## Known limitations

- USB input and 0x02 output fields (LEDs, rumble, triggers) work. USB speaker
  PCM is routed to the controller's ALSA UAC endpoint via `aplay`; USB
  microphone capture is not routed. The raw 0x36 speaker and 0xD4 mic paths are
  Bluetooth-only.
- The idle fade floor is fixed at 10%.
- Mic-monitor state is not persisted.
- Playhead saves every 30 s while playing; volume saves after 1 s of quiet.
- Jack pause is API/decoder verified; a physical unplug has not been observed.
- `/api/reload` is a deliberate no-op and `hid.power_save` is null.

## Protocol

`dualsense.hh` is the core wire description. `dualsense-extra.hh` holds command,
feature-report, authentication and USB-topology metadata.
