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
padwave.cc        the engines and main: wire include, player, input decoder,
                  opusdec, main
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

GCC 12 has no `#embed`, so the build generates `padwave_ui.hh` from
`index.html`; a compiler with `#embed` (Clang 19+) can use the file directly.

## Formatting

The C++ sources are formatted with `.clang-format`: LLVM, 100 columns, 2-space
indent, private class fields, compact one-line guards. Run:

```sh
clang-format -i padwave.cc padwave_app.hh padwave_server.hh padwave_wire.hh test_wire.cc
```

## Build and run

Needs a C++ compiler, `pkg-config`, the FFmpeg development packages
(`libavformat`, `libavcodec`, `libswresample`, `libavutil`), `libopus`, `zlib`
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
rumble/trigger release. Config persists to `~/.config/padwave/config`.

Not yet ported from the old Elixir app: idle fade/suspend and controller
power-off, persisted playhead, and mic level decoding (the monitor switch
returns "not implemented" rather than pretending).

## Protocol

The wire formats are described, with their bit positions, in
`padwave_wire.hh`; `docs/protocol.md` has the longer notes. No other
file describes the packets.
