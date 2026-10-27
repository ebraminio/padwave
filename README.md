# padwave

A controller-driven player for the Raspberry Pi. One DualSense over Bluetooth, a
folder of audio files, and the controller's own headphone jack as the speaker.

A station is a file, so anything that sits in a folder already works: radio
rips, long mixes, albums, podcast feeds you have already downloaded. Online
feeds and live streams are the next step, not today's.

```
DualSense ──BT──▶ hidraw ──▶ decode ──▶ actions ──▶ radio ──▶ player ──▶ 0x36
   ▲                                                              │
   └──────────── lightbar, rumble, triggers (0x31) ◀──────────────┘
```

One binary, one process, one thread. `padwave serve` is the app: it scans the
station folders, owns the radio and the panel, and drives the player (timerfd +
ring: decode, crossfade, gapless) and the input decoder (the hidraw fd) from the
same epoll loop. The `native`, `input` and `opusdec` modes remain for debugging,
but the service is a single PID with a single thread. There is no Mix/Elixir.

## Controls

| Input | Action |
|---|---|
| L2 / R2 | volume down / up — deeper press, faster change |
| d-pad ← / → | seek −6 s / +6 s; hold to steer, ramping 10 → 20 → 40 → 60 s |
| d-pad ↑ / ↓ | next station / previous station |
| △ | pause / resume |
| ○ | play (stands in for this controller's dead ✕) |
| □ | another game's folder, random station |
| L1 / R1, Create / Options, L3 / R3 | previous / next station |
| touchpad, mute | mute / unmute |
| PS held ~2 s | save the playhead and power the controller off |
| Edge paddles, Fn buttons | previous / next station, pause, mute |

Analogue sticks are deliberately unbound: ours are worn and jitter, so they do
the one thing they are good for — being ignored.

## Layout

```
padwave.cc        the player and main
padwave_input.hh  struct Input: hidraw, callbacks, decoder and the input CLI
padwave_app.hh    struct App: library, playback, controller snapshot, config
                  and lab output (inline methods, private fields)
padwave_server.hh struct Server: epoll, HTTP routes, SSE, embedded UI
padwave_wire.hh   the wire formats (number types + DualSense schemas)
index.html        the panel, embedded into the binary at build time
test_wire.cc      wire layout and CRC (compiled and run on every build)
build.sh          the build: ui header, wire test, then the binary
install.sh        build + install + systemd unit
padwave.service, padwave-boot.sh, padwave-ctl, deploy.sh
```

## Formatting

The C++ sources are formatted with `.clang-format`: LLVM, 100 columns, 2-space
indent, private class fields, compact one-line guards. Run:

```sh
clang-format -i padwave.cc padwave_app.hh padwave_server.hh padwave_wire.hh test_wire.cc
```

## Build and run

Needs a C++ compiler, `pkg-config`, the FFmpeg development packages
(`libavformat`, `libavcodec`, `libswresample`, `libavutil`) and `libopus`
and `python3` for the UI header.

```sh
./build.sh
PADWAVE_ROOTS=/path/to/audio PADWAVE_STANDALONE=1 ./padwave serve
# panel: http://<pi>:8787
```

`PADWAVE_STANDALONE=1` turns on the app's own `/api/*` routes; the unit sets it.
`PADWAVE_ROOTS` is a colon-separated list scanned recursively; without it the app
uses `~/Music`.

## Install

On the Pi, from the project root:

```sh
./install.sh
```

It builds, installs `~/.local/bin/padwave`, installs the systemd user unit and
restarts it. The panel is on port 8787, LAN-reachable, no auth (keep it on a
trusted network).

## Pi Bluetooth

If `bluetoothd` sits at a few percent CPU while the controller is idle, it is
probably relaying HID in userspace. BlueZ 5.82 defaults to `UserspaceHID=true`;
set `UserspaceHID=false` in `/etc/bluetooth/input.conf`, load the `hidp` module,
and restart `bluetooth.service`. Kernel HIDP keeps the 500 Hz report path out of
bluetoothd (measured ~3 % → ~0.1 %).

## Deploy

From the Mac, sync the source tree (not the Mac binary) and rebuild/install on
the Pi:

```sh
PADWAVE_HOST=ebrahim@192.168.1.175 ./deploy.sh
# override the remote directory with PADWAVE_DIR=... if needed
```

The manual equivalent:

```sh
rsync -az --delete --exclude '.git' --exclude 'padwave' \
  --exclude 'var' ./ ebrahim@192.168.1.175:~/padwave/
ssh ebrahim@192.168.1.175 'cd ~/padwave && ./build.sh && ./install.sh'
```

`install.sh` installs `~/.local/bin/padwave` and restarts the user unit. To
deploy an older commit, check it out locally and run the same commands.

## HTTP

Every route speaks `key=value` lines: dotted keys nest, and
`true`/`false`/`null` are literals. `GET /api/status`, `GET /api/events` (SSE,
5 Hz), `GET /api/front` and `/api/front/events` are the state stream.
`POST /api/command` takes `action=next` plus `value`, `delta`, `position` or
`id`, and answers `ok=true` or `ok=false` + `error=...`; the commands are
`next`, `previous`, `pause`, `mute`, `shuffle_game`, `disconnect`, `volume`,
`seek`, `seek_to`, `play_track`. `GET /api/library` is one tab-separated line
per track (id, title, game, path, ext); `GET /api/config` and `POST
/api/config` are the persisted settings; `/api/schema` is `bindings.*`;
`POST /api/output` is the controller lab.

## Status

Working in the single binary: station scan and shuffle, history, play/pause,
next/previous, shuffle-game, seek, volume/mute, gapless and crossfade in the
player, the panel with controller state and SSE, and the LAN API.

The controller lab works through the player's `fields` reports (lightbar,
player LEDs, power save, rumble, trigger effects, raw field maps), with timed
rumble/trigger release. Trigger effects use the eight DS4Windows Trigger Lab
presets or a custom 0x26 vibration; the selected preset and custom
position/amplitude/frequency are also used by the "on volume" buzz and both
test buttons. Config, volume and the playhead persist to
`~/.config/padwave/config`; the app resumes the saved station, offset and
volume.

The lightbar row has the normal colour/brightness controls, a one-shot
**light out**, and a host-driven **pulse**. `lightbar_setup` is gated by
`valid_flag2` bit 1 and value `0x02` merely releases/blank the bar; the
controller has no programmable blink. Smooth pulsing is repeated RGB updates
from the host.

Idle fades the volume over the configured grace and suspends playback; any
controller action, event, command or config edit restores the volume and
resumes. After `poweroff_when_idle_ms` the app persists and asks BlueZ to
disconnect the pad, which puts it to sleep; press PS to wake it. Unplugging the
headset pauses and plugging it back resumes only if the jack caused the pause.
The monitor switch opens the 0x36 mic path, and the status stream carries a live
Opus-decoded level (frames, RMS, peak). Microphone monitoring and playback are
mutually exclusive: pausing with triangle, the touchpad/mute, the pause command
or zero volume turns the mic on; resuming turns it off. Enabling the mic
directly pauses playback, and switching it off resumes only when the mic caused
that pause. Touchpad mute records the pause as volume-caused, so its next click
unmutes and resumes.

Volume zero *is* the paused state: reaching 0 pauses, raising it again or
pressing play/mute restores the last nonzero volume, and a new controller
connection never leaves a paused zero-volume session. The current and last
non-zero volumes are persisted and restored on restart; 15% is used only when
no non-zero value has ever been stored. A change is saved after a second of
quiet, and immediately on shutdown or `/api/config`. The player's volume and
pause state are applied on every player start.

The lightbar follows playback: cyan playing, amber paused, purple muted,
pulsing red for the idle warning, and black when suspended/off. A lab custom
colour overrides it until the next state change. Charging/full sweeps the five
player LEDs; otherwise the kernel player-1 pattern is used.

If the controller disconnects there is no audio sink, so playback pauses and
the player tick slows to 500 ms; that keeps a controller-less unit near 0 % CPU
(measured 0.00 % over 8 s). The first frame after the pad reconnects resumes
playback. With the controller connected, the ~500 Hz input stream still costs
roughly 0.9 % of a core even while paused; that is the input engine, not audio.

Not yet done: real audio-sink/state reporting (the panel still says no sink),
configurable idle fade floor, persisted mic-monitor state, and physical
confirmation of the jack path (lightbar, rumble and trigger effects are
confirmed).

## Protocol

The wire formats are described, with their bit positions, in
`padwave_wire.hh`; `docs/protocol.md` has the longer notes. No other
file describes the packets.
