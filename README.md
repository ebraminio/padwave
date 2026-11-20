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

padwave registers a PipeWire sink called `padwave` at startup. It is visible to
other PipeWire clients and becomes the default sink under the normal single-sink
WirePlumber policy. When something starts playing into it, the local file player
stops and the existing timer/Opus/HID path forwards the system audio to the
controller. Pressing a local playback control switches back to the file player;
the sink node remains available. PipeWire and WirePlumber must be running.

The **Live** tab is optional. Paste an M3U/M3U8 playlist URL into the tab, save
it, and select a channel; padwave saves the URL in its config and starts one
`ffmpeg` process. That process resolves the master playlist to its first
(lowest-bitrate) HLS variant and writes local HLS segments; `mpv` plays those
local segments for the controller audio, while a small static HTTP server exposes
the same HLS on port 8090 for VLC. Only ffmpeg reads the upstream stream, so
bandwidth is not doubled. No playlist URL is compiled into the binary. Open
`http://<pi>:8090/index.m3u8` in VLC or ffplay; mute VLC if you are listening
through the controller. Install `mpv` at runtime with `sudo apt install mpv`.
The tab also shows channel logos, radio markers, provider/country metadata, a
direct-URL field, and an embedded muted HLS player using native HLS (Safari).
The VLC link remains available for other browsers.

## Build and run

Needs a C++23 compiler (`clang`), `pkg-config`, the FFmpeg development
packages (`libavformat`, `libavcodec`, `libswresample`, `libavutil`,
`libavfilter`), `libopus-dev`, `libpipewire-0.3-dev`, and on Linux
`libbluetooth-dev`.

On Raspberry Pi OS or Debian:

```sh
sudo apt update
sudo apt install clang pkg-config build-essential \
  libavformat-dev libavcodec-dev libswresample-dev libavutil-dev libavfilter-dev \
  libopus-dev libbluetooth-dev libpipewire-0.3-dev
```

`bluez` is needed at runtime to pair the controller, but is not a build
dependency. `hcitool` is not needed.

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

## Standalone demo

`dualsense-demo.cc` is an independent hidapi client that includes only
`dualsense.hh`. Opus and HIDAPI are Git submodules pinned to release tags;
xmake compiles them directly, with no Meson, CMake, or package manager:

```sh
git clone --depth 1 --recurse-submodules --shallow-submodules <repo>
# or in an existing checkout:
git submodule update --init --depth 1

cd dualsense-demo
xmake f -c -y
xmake -y
xmake test
xmake run dualsense-demo --help
```

On Windows use xmake with Visual Studio/MSVC installed; xmake detects it
automatically:

```bat
cd dualsense-demo
xmake f -c -y
xmake -y
xmake test
xmake run dualsense-demo --help
```

Linux needs `libudev-dev` for HIDAPI’s hidraw backend. Opus is compiled from
`third_party/opus` with source globs; HIDAPI is a native static target from
`third_party/hidapi`. The result has no build-tree DLLs on Windows.

With no pipe the demo displays controller input and accepts terminal keys. Pipe
raw `s16le`, 48 kHz, stereo PCM to stream to the Bluetooth speaker:

```sh
ffmpeg -re -i track.mp3 -f s16le -ac 2 -ar 48000 - | xmake run dualsense-demo
```

On Windows, feed the binary pipe through `cmd.exe`; PowerShell's text pipeline
corrupts raw PCM with line-ending and encoding changes:

```cmd
cmd /c "ffmpeg -re -i track.flac -f s16le -ac 2 -ar 48000 - | .\build\windows\x64\release\dualsense-demo.exe"
```

Writing the PCM to a file and using `< pcm.raw` works too.

Output-report flags:

```sh
xmake run dualsense-demo --lightbar 0,80,255 --leds 4 --trigger left,2,6 --rumble 120,120,300
```

With a terminal and no pipe, `l`, `r`, `p`, `t`, `h`, and `q` exercise the
lightbar, rumble, player LEDs, trigger feedback, help, and quit. Stop the
padwave service first; two clients reading the same hidraw will split input.

The 0x36 streaming path is Bluetooth-only. On USB the demo still provides input,
lightbar, player LEDs, triggers, and rumble, but no speaker audio.

## ds2xi

`ds2xi/` is a Windows-only XInput wrapper built directly on `dualsense.hh`. It
exports `XInput1_4.dll`, maps the first DualSense to player 0, and falls through
to the system XInput for every other index.

```bat
cd ds2xi
xmake f -c -y
xmake -y
```

It uses HIDAPI from `../dualsense-demo/third_party/hidapi`, so initialize the
demo submodules first. The build produces
`build/windows/x64/release/XInput1_4.dll`.

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
`GET /api/library` and `/api/live` are TSV; `/api/config` persists settings;
`/api/schema` describes the lab fields; `POST /api/output` is the controller lab.
The live commands are `live_url` (value), `live_refresh`, `live_play` (id),
`live_direct` (value) and `live_stop`; ffmpeg fetches/remuxes the channel once to
local HLS, and mpv plays that local HLS for the controller. The parsed playlist
is cached at `~/.cache/padwave/live.m3u`, so opening the tab does not refetch it.

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
