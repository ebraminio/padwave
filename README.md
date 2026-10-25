# padwave

A controller-driven radio for the Raspberry Pi.

Point a DualSense at a folder of GTA radio stations, steer through them with the
d-pad, and let the controller's own headphone jack be the speaker. When you walk
away it fades itself down and switches the controller off; when you pick it up
again the station is exactly where you left it.

```
DualSense ──BT──▶ hidraw ──▶ packet decode ──▶ actions ──▶ Radio ──▶ mpv ×2 ──▶ PipeWire
   ▲                                                          │                  │
   └──────── lightbar + triggers (0x31) ◀── state ◀── web UI ◀───┘                  │
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

Defaults are AntennaPod's: a **15 minute** timer whose last **10 seconds** are
the fade (`NOTIFICATION_THRESHOLD = 10000`, `max(0.1, remaining / 10 s)`).

* 15 minutes with no real input → the sleep timer arms and the fade begins
  immediately: the lightbar pulses red, the web UI counts down, and the volume
  ramps linearly to 10 %. Raise `idle_warn_grace_ms` (UI: "warn") if you want
  the arming light and countdown to last longer than the fade.
* At zero → music fades out at the floor, the exact playhead is saved, and the
  controller is disconnected (which puts it to sleep).
* Any button, trigger or d-pad press during the warning cancels the shutdown and
  restores the volume — and so does **shaking the controller** harder than
  2.25 g, the same threshold AntennaPod's `ShakeListener` uses.
* Controller switched back on (PS button) → the same station resumes at the same
  second.

The playhead is checkpointed every 15 seconds while playing and written
synchronously when the service stops, so a `systemctl restart` (or a reboot)
resumes the same station at the same second. A session that was *paused* comes
back paused, on the right station and second — nothing starts playing by
itself.

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

Declaring the description at the wrong size is a compile error. Multi-byte
integers name their byte order (`i16le`, `u32le`) so the description says what
it means, and `skip n` would keep gaps honest if there were any left — there
are not.

The schema files are excluded from `mix format`: the trailing comments and the
column layout *are* the documentation, and the formatter reflows a trailing
comment onto the line above.

Every field carries a trailing comment with its offset and meaning, and the
same file is served at `GET /api/schema/source` so the UI can show the protocol
documentation next to the live values — the description is not a separate
document that can drift.

The output side (`lib/padwave/schemas/dualsense_output.ex`,
`lib/padwave/output/report.ex`) uses the same mechanism to build lightbar and
player-LED reports, including the Bluetooth CRC. That encoder is tested against
a packet captured from the kernel itself with `btmon`, so the layout and the
checksum are verified rather than guessed:

```
31 e0 10 00 04 00 … be … fc 54 72 1b     ← kernel
31 e0 10 00 04 00 … be … fc 54 72 1b     ← Report.build/1
```

### Same description, two transports

USB and Bluetooth carry the same 63-byte input payload and the same 47-byte
output payload; only the framing differs. Each payload is described once, with
a trailing comment per field, and referenced by both packets in the same file:

```elixir
field_list :input_body do
  u8 :lx                 # +0x00  left stick X, 0..255 (128 = centre)
  ...
end

packet :input, 0x31, size: 78, doc: "Bluetooth" do
  u8 :seq_tag            # 0x01  high nibble = sequence tag
  splice :input_body
  bytes :bt_reserved, 9
  u32 :crc               # 0x4a  seed 0xA1
end

packet :input_usb, 0x01, size: 64, doc: "USB" do
  splice :input_body
end
```

The splice is resolved at compile time, so the decoder is still one binary match
over the whole report. `+0x00` in the comments is the offset inside the payload,
which is why one description serves both transports.

The device reads the bus from `HID_ID` (`0x03` USB, `0x05` Bluetooth), picks the
matching spec for both the report size and the decoder, so nothing above the
input layer knows or cares. Tests assert that every shared field's USB offset is
exactly its Bluetooth offset minus the header. The output encoder takes
`bus: :usb` (no CRC — only the Bluetooth transport is signed).

The one honest caveat: LinuxAudio4Dualsense5 publishes a *Bluetooth* sink, so a
wired controller would currently play to a normal output until that project
gains USB audio. The padwave side needs no change.

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

`install.sh` builds with `MIX_ENV=prod`, installs a systemd user unit, and
starts it. `sudo
loginctl enable-linger $USER` is required for it to start at boot (the installer
assumes it; run it once).

Requirements: `mpv`, `pipewire` + `pipewire-pulse`, Elixir ≥ 1.14, and
[LinuxAudio4Dualsense5](https://github.com/GeorgLegato/LinuxAudio4Dualsense5)
providing the `ds5_membrane` sink (otherwise point `audio_device` at any sink).

Configuration lives in `config/config.exs`: music roots, crossfade length, idle
timeouts, web port, and which LED backend to use.

## Design notes

**One writer for output reports.** Lightbar and trigger effects both go through
`Padwave.Output.Hid`, which is the only thing in padwave writing report `0x31`,
and it owns the Bluetooth sequence tag and CRC. The kernel's `playstation`
driver still owns the rumble path, but it exposes neither adaptive triggers nor
their actuators, so those bytes are ours. The `valid_flag*` bits keep the two
worlds apart: we only ever set the flags for the fields we touch, and
LinuxAudio4Dualsense5's `0x36` audio stream is a different report entirely.

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

## Why not speak the speaker protocol ourselves?

The tempting move is to drop LinuxAudio4Dualsense5 and write report `0x36` from
Elixir. Having read what it takes, that would be the wrong trade:

* the audio is **Opus, 48 kHz stereo, 200-byte frames, every 10.667 ms** — a
  jitter budget of a couple of milliseconds. The BEAM is many good things, but
  it is not a real-time pacer; you would end up writing a C port to hit that
  deadline, at which point you have rebuilt the thing you replaced;
* the sink also has to get the **config mask `0xFE`** exactly right (bit 0 turns
  on duplex/microphone capture and starves the speaker), mute the mic in the
  state snapshot, and re-enable the audio path (`0x31`, `audio_control 0x30`)
  after every reconnect;
* it is **not a collision anyway**. That service writes `0x36` audio; padwave
  reads `0x31` input and writes only its own output fields. The
  kernel already multiplexes those. Two writers only becomes a problem if both
  write the *same* report type.

So the boundary stays at "push PCM into a PipeWire sink". We keep the health of
that boundary visible instead (`/api/status` → `audio`: sink present, state,
and which padwave streams are on it), because a dead sink is the one failure
that looks like silence rather than an error.

## Why HTTP and not Erlang distribution?

Distribution is genuinely nice and, for a BEAM-to-BEAM link, strictly more
powerful: no schema, `GenServer.call({Padwave.Radio, node}, ...)` just works, and
you can attach a remote shell to the live radio. But it is the wrong *product*
interface:

* the browser is not a BEAM node — HTTP is the only thing the web UI can use
  anyway, so the API exists regardless;
* distribution needs a shared cookie and resolvable node names (`-name`/`-sname`
  and, on Debian, the hostname landing on `127.0.1.1`), and it fails in ways that
  are opaque from a laptop;
* the cookie is effectively a root-equivalent secret: a connected node can
  `:rpc.call/4` anything. Fine between two of your own machines on a home LAN,
  a poor default for something with a web UI on `0.0.0.0`.

The split that makes sense: **HTTP is the API, distribution is the operator
channel.** To use the latter, start the node with a name in the unit:

```ini
Environment=ELIXIR_ERL_OPTIONS=+fnu -name padwave@rpi5-secondary.local
```

then from the Mac (with the Pi's `~/.erlang.cookie` copied over):

```bash
iex --name me@my-mac.local --cookie ~/.erlang.cookie --remsh padwave@rpi5-secondary.local
```

That drops you into the running radio with full introspection — which is also
the fastest way to iterate on the packet description while it is live.

## Talking to it from the Mac

The controller can only be paired to one host at a time, so the Pi keeps the
controller and everything else talks to the Pi. `scripts/padwave-ctl` is the
smallest useful bridge (it is plain curl + `python3 -m json.tool`):

```bash
scripts/padwave-ctl status
scripts/padwave-ctl vol -5
scripts/padwave-ctl next
```

To make the Mac's volume keys drive the controller's volume, mirror the Mac's
output volume with Hammerspoon:

```lua
local dev = hs.audiodevice.defaultOutputDevice()
local last = -1
hs.timer.doEvery(0.5, function()
  local v = math.floor(dev:volume())
  if v ~= last then
    last = v
    hs.execute("~/padwave/scripts/padwave-ctl volset " .. v)
  end
end)
```

That is a one-way mirror and will fight the controller's own L2/R2 changes; the
honest version is a small menu-bar app that treats the two as one control with
a short "recently changed by me" debounce. Same API, just more state.

A tray/menu-bar companion (CPU, temperature, battery, now playing, transport)
is the obvious next step: `/api/events` is already an SSE feed of exactly the
data such an app needs, so it can be a Swift menu-bar app, a Hammerspoon script,
or a tiny Elixir `escript` — no new server surface required.

## Next iteration

The Pi owns the controller, so a laptop cannot be paired to it at the same time.
The realistic shape for "my Mac's volume keys move the controller's volume" is
the reverse: a small Mac-side agent (Hammerspoon/`skhd`, or a tiny Elixir
program) posting to `POST /api/command` on the Pi — the API is already the right
seam. Streaming the Mac's audio to the controller would need the Pi to accept an
audio stream (e.g. an RTP/PipeWire module or an Opus source feeding the
`ds5_membrane` sink) and is a genuinely separate project.
