# padwave

A controller-driven radio for the Raspberry Pi. One DualSense over Bluetooth, a
folder of GTA radio stations, and the controller's own headphone jack as the
speaker.

```
DualSense ──BT──▶ hidraw ──▶ decode ──▶ actions ──▶ Radio ──▶ mpv ×2 ──▶ PipeWire
   ▲                                                     │                  │
   └──── lightbar + triggers (0x31) ◀── state ◀── web UI ◀┘                  │
                                                     ds5_membrane sink ◀─────┘
```

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

Analogue sticks are unbound on purpose: this controller's are worn and jitter.

## Idle

Fifteen minutes without real input fades the volume to 10 % over the last ten
seconds, then saves the playhead and disconnects the controller. Any button,
trigger or a shake above 2.25 g cancels the shutdown and restores the volume.
Sticks do not count as activity. A session that was paused comes back paused.

## Install

```bash
git clone <this repo> ~/padwave
cd ~/padwave
./scripts/install.sh          # builds, installs a systemd user unit, starts it
sudo loginctl enable-linger $USER   # once, so it starts at boot
```

Needs `mpv`, `pipewire` + `pipewire-pulse`, Elixir ≥ 1.14, and
[LinuxAudio4Dualsense5](https://github.com/GeorgLegato/LinuxAudio4Dualsense5)
for the `ds5_membrane` sink (or point `audio_device` at any sink).

## Web UI

`http://<pi>:8787` — transport, seek bar, volume, stations, the live decoded
packet, the bindings, and a controller lab for trying output-report fields by
hand.

| Endpoint | |
|---|---|
| `GET /api/status` | radio, session, controller, bindings, audio |
| `GET /api/events` | the same as SSE, ~200 ms |
| `GET /api/library` | stations |
| `GET /api/schema` | decoded packet layouts as data |
| `POST /api/command` | `{"action":"pause"}`, `{"action":"volume","value":40}` |
| `POST /api/config` | runtime settings (JSON, form or query) |
| `POST /api/reload` | hot-swap changed modules |
| `POST /api/debug/frame` | inject a press, e.g. `{"buttons":["triangle"]}` |
| `POST /api/output` | lightbar, player LEDs, trigger effects, raw fields |

## Configuration

`config/config.exs` holds the music roots, crossfade, idle timeouts, web port and
trigger-effect defaults. Volume, idle minutes, warning seconds and trigger feel
are also settable at runtime from the UI.

## From the Mac

The controller can only be paired to one host, so the Pi keeps it and everything
else talks to the Pi:

```bash
scripts/padwave-ctl status
scripts/padwave-ctl vol -5
scripts/padwave-ctl next
```

## Layout

```
lib/padwave/
  wire.ex                 the packet DSL (bits, enums, splices, CRC framing)
  schemas/                the protocol, as source: input, output, audio, feature
  input/                  device discovery, decode, mapper, simulator
  output/                 HID writer, lightbar, trigger effects, lab
  player/                 two mpv slots and their crossfade
  radio.ex                stations, shuffle bag, queue, checkpoints
  session.ex              idle timer and shutdown
  web/                    router and SSE
priv/ui/                  the web UI
docs/protocol.md          every report id we know about, and what is a guess
```

## Protocol

The packet descriptions are the documentation: every field carries its offset
and a comment, and `GET /api/schema` returns the same layout as data. The
catalogue of report ids — including what is still unknown — is in
[docs/protocol.md](docs/protocol.md).

## Tests

```bash
mix test
```

Covers packet decoding against captured reports, the output encoder against a
captured kernel packet, CRC, the shuffle bag, the idle timer, and end-to-end
button mapping through the report simulator.
