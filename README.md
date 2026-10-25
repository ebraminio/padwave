# padwave

A controller-driven radio for the Raspberry Pi.

Point a DualSense at a folder of GTA radio stations, steer through them with the
d-pad, and let the controller's own headphone jack be the speaker. When you walk
away it fades itself down and switches the controller off; when you pick it up
again the station is exactly where you left it.

```
DualSense ──BT──▶ hidraw ──▶ packet decode ──▶ actions ──▶ Radio ──▶ mpv ×2 ──▶ PipeWire
   ▲                                                          │                  │
   └──────── lightbar (sysfs) ◀── state ◀── web UI ◀────────────┘                  │
                                                          ds5_membrane sink ◀──────┘
                                                          (LinuxAudio4Dualsense5)
```

## What it does

| Input | Action |
|---|---|
| **L2 / R2** (analogue) | volume down / up — deeper press, faster change |
| **d-pad ← / →** | seek −6 s / +6 s; **hold to steer**, ramping 10 → 20 → 40 s per step |
| **d-pad ↑** | next station in the folder (bag shuffle: every station once before repeating) |
| **d-pad ↓** | previously played station (history) |
| **△ triangle** | pause / resume |
| **○ circle** | play / resume — the stand-in for this controller's dead ✕ |
| **□ square** | switch to another game's folder, random station from it |
| **L1 / R1** | previous / next station |
| **Create / Options** | previous / next station |
| **touchpad click** | mute / unmute |
| **PS held ~2 s** | stop, remember the playhead, power the controller off |

Analogue sticks are deliberately unbound: ours are worn and jitter, so they do
the one thing they are good for — being ignored.

### Idle behaviour

* 15 minutes with no real input → volume is raised as a warning, lightbar pulses
  red, web UI counts down.
* 1 more minute → music fades out, the exact playhead is saved, and the
  controller is disconnected (which puts it to sleep).
* Any button, trigger or d-pad press during the warning cancels the shutdown and
  restores the previous volume.
* Controller switched back on (PS button) → the same station resumes at the same
  second.

Sticks do not count as activity by default (see `analog_wake_delta`); triggers
do, because they rest at zero and are not noisy.

## Why Elixir

The original question was whether a mostly-idle service is a good fit for the
BEAM. It is, for three reasons that matter here:

1. **Idle costs nothing.** A process blocked on a blocking `read/2` on
   `/dev/hidraw0`, or waiting on a timer, consumes no scheduler time. The whole
   VM idles at roughly 0.1 % CPU and ~60 MB — on an 8 GB Pi 5 that is free.
   "Idle" is the BEAM's happy path, not its weakness.
2. **Binary pattern matching *is* a packet description language.** Describing a
   controller report is the language's native idiom, not a library you have to
   adopt.
3. **Hot swap is real.** `Code.compile_file/1` into a running node, purge the old
   version, and the next controller report is decoded by the new code — with the
   music still playing.

## The packet DSL

Packets are described, not hand-parsed. `lib/padwave/schemas/dualsense_input.ex`:

```elixir
use Padwave.Wire

packet :input, 0x31, size: 78 do
  u8 :seq_tag
  u8 :lx
  ...
  bits :buttons0, dpad: 4, square: 1, cross: 1, circle: 1, triangle: 1
  ...
  i16 :gyro_x
  ...
  u8 :status0        # battery level + charging state
  u8 :status1        # bit 0 = headset plugged in
  ...
  u32 :crc
end
```

Each description compiles into

* `decode_input/1` — a single binary match, no copying, with the declared size
  checked at compile time;
* `spec(:input)` — the same layout as plain data, which the web UI renders and
  which the encoder uses to find field offsets.

Declaring the description at the wrong size is a compile error, and `skip n`
keeps the offsets honest about the bytes we have not decoded.

The output side (`lib/padwave/schemas/dualsense_output.ex`,
`lib/padwave/output/report.ex`) uses the same mechanism to build lightbar and
player-LED reports, including the Bluetooth CRC. That encoder is tested against
a packet captured from the kernel itself with `btmon`, so the layout and the
checksum are verified rather than guessed:

```
31 e0 10 00 04 00 … be … fc 54 72 1b     ← kernel
31 e0 10 00 04 00 … be … fc 54 72 1b     ← Report.build/1
```

## Hot swap

`Padwave.HotSwap` watches `lib/**/*.ex` and `priv/**/*.ex` and recompiles what
changed into the running node, purging the previous version:

```bash
$ curl -X POST localhost:8787/api/reload
{"ok":true,"reloaded":["Padwave.Schema.DualSenseInput"]}
```

Edit a packet description, a button binding, or the web page, and the next
report uses it — no restart, no dropped audio. (`lib/padwave/schemas/` and
`priv/ui` are the interesting files; state-shape changes in a GenServer are the
one thing to be careful with.)

## Web UI

`http://<pi>:8787` — now playing, seek bar, volume, transport, the station list,
the live decoded packet (hex plus fields), and a controller panel that lights up
as buttons are pressed.

API:

| Endpoint | |
|---|---|
| `GET /api/status` | radio, session, controller, bindings |
| `GET /api/library` | all stations |
| `GET /api/schema` | the input packet spec + button bindings |
| `POST /api/command` | `{"action": "pause"}` etc. |
| `POST /api/reload` | hot-swap changed modules |
| `POST /api/debug/frame` | inject a synthetic press, e.g. `{"buttons":["triangle"]}` |

The debug endpoint is how the button map is tested without a human: it builds a
real 0x31 report, pushes it through the normal decode path, and suppresses the
live device frames for the duration of the press so holds behave like holds.

## Install

On the Pi:

```bash
git clone <this repo> ~/padwave     # or: ./scripts/deploy.sh from the Mac
cd ~/padwave
./scripts/install.sh
```

`install.sh` builds with `MIX_ENV=prod`, installs the udev rule that lets the
user write the lightbar, installs a systemd user unit, and starts it. `sudo
loginctl enable-linger $USER` is required for it to start at boot (the installer
assumes it; run it once).

Requirements: `mpv`, `pipewire` + `pipewire-pulse`, Elixir ≥ 1.14, and
[LinuxAudio4Dualsense5](https://github.com/GeorgLegato/LinuxAudio4Dualsense5)
providing the `ds5_membrane` sink (otherwise point `audio_device` at any sink).

Configuration lives in `config/config.exs`: music roots, crossfade length, idle
timeouts, web port, and which LED backend to use.

## Design notes

**Lightbar via the kernel, not raw HID (by default).** The kernel's
`playstation` driver already speaks the output-report protocol — including the
Bluetooth sequencing and CRC — and exposes the very same lightbar as
`/sys/class/leds/*:rgb:indicator`. Writing our own `0x31` reports while
LinuxAudio4Dualsense5 streams audio in `0x36` reports means two processes owning
one state machine; the `valid_flag*` bits make it *safe* (unset flags are left
alone), but it is still a second writer for no benefit when the LED class exists.
The packet encoder is implemented and tested so the HID path is a config flag
away, not a rewrite.

**Headset pause button.** The DualSense's jack has no HID path for inline
headset buttons: the controller exposes a gamepad, motion sensors and a
touchpad, and the only jack information in report `0x31` is "headphones plugged
in" (status byte 1, bit 0) plus "microphone" (bit 1). There is no consumer
control collection, and BlueZ does not surface one for this device, so a pause
button on the cable is almost certainly handled inside the headset and never
reaches the host. If it turns out to toggle any bit, `POST /api/debug/frame` and
the raw packet panel make it easy to spot and bind.

**Analogue noise.** Stick drift is expected on this controller; wake-up
detection therefore keys off buttons, d-pad and triggers only, with a delta
threshold for the triggers. Sticks can be opted in with `analog_wake_delta`.

**Audio.** Two `mpv` instances pinned to the `ds5_membrane` sink; a station
change is a 1.5 s crossfade in which one slot fades out while the other fades
in, driven by a 40 ms volume ticker (`master × gain`). Pinning the sink means a
system default-sink change cannot steal the music.

## Tests

```bash
mix test
```

24 tests: packet decoding against a captured report, the output report against a
captured kernel packet, battery/headset/axis derivations, the shuffle bag, and
end-to-end button mapping through the simulator.

## Next iteration

The Pi owns the controller, so a laptop cannot be paired to it at the same time.
The realistic shape for "my Mac's volume keys move the controller's volume" is
the reverse: a small Mac-side agent (Hammerspoon/`skhd`, or a tiny Elixir
program) posting to `POST /api/command` on the Pi — the API is already the right
seam. Streaming the Mac's audio to the controller would need the Pi to accept an
audio stream (e.g. an RTP/PipeWire module or an Opus source feeding the
`ds5_membrane` sink) and is a genuinely separate project.
