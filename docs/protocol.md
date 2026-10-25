# DualSense protocol catalogue

What is known about the controller's report ids, what padwave does with each,
and where the knowledge comes from. The descriptions themselves live in
`lib/padwave/schemas/` and are served as data at `GET /api/schema`.

## Framing

A report is `report id` followed by its payload. Over Bluetooth the payload is
signed; over USB it is not.

| | Bluetooth | USB |
|---|---|---|
| Input | `0x31`, 78 bytes, CRC-32 seed `0xA1` at the end | `0x01`, 64 bytes |
| Output | `0x31`, 78 bytes, seed `0xA2`, tag byte `0x10` at `[2]` | `0x02`, 48–63 bytes |
| Feature | seed `0xA3` | — |

The payload is the same in both transports; only the framing differs, which is
why the kernel parses both with one `struct dualsense_input_report`.

A note on the USB output length: the kernel builds 63 bytes (`report id + 47
payload + 15 reserved`) and the descriptor in DualSenseClient suggests 48 (`id +
47`). Our schema follows the kernel; nothing in padwave sends over USB yet.

## Reports

| id | dir | length | what it is | padwave |
|---|---|---|---|---|
| `0x01` | in | 64 | full input report, USB | decoded (`:input_usb`) |
| `0x01` | in | 10 | *minimal* input report over Bluetooth | ignored (the full one is `0x31`) |
| `0x31` | in | 78 | full input report, Bluetooth | decoded (`:input`) |
| `0x31` | out | 78 | state report: LEDs, player LEDs, rumble, triggers, audio routing | driven (lightbar, trigger actuators, lab) |
| `0x02` | out | 48–63 | state report, USB | encoded (`:output_usb`), unused |
| `0x36` | out | 398 | audio: config + state + haptics + Opus speaker | described (`:audio_out`), owned by LinuxAudio4Dualsense5 |
| `0x31` + | in | 78 | microphone Opus inside the input report, marker `0xD4` | not used; the audio service mutes it |
| `0x32` | out | 78 | haptics-only variant used in the audio project's early experiments | documented only |
| `0x05` | feat | 41 | motion-sensor calibration: 17 × int16 LE | described (`:calibration`) |
| `0x09` | feat | 20 | pairing info: MAC at `[1..6]` | described (`:pairing`) |
| `0x20` | feat | 64 | firmware/hardware info: hw `[24]`, fw `[28]`, update `[44]` | described (`:firmware`) |
| `0x80` | feat | — | vendor command write | documented only |
| `0x81` | feat | 64 | vendor command response | documented only |
| `0x65` | feat | 64 | DualSense Edge factory handshake | documented only |
| `0x60`–`0x7B` | feat | — | Edge profile blocks | not touched |

## Details worth remembering

**Input `0x31`.** Buttons, sticks, both triggers, gyro, accelerometer, the
sensor timestamp, temperature, two touch points, adaptive-trigger feedback,
host/device timestamps, battery + charging, headset and microphone detection.
Every byte is named in `Padwave.Schema.DualSenseInput.Body`.

**Output `0x31`.** Applied field-by-field according to `valid_flag0/1/2`: a
report only changes what it flags, which is why padwave's lightbar and trigger
updates coexist with the audio service's stream. Rumble is `valid_flag0` bit 0
and haptics bit 1 — deliberately left to the audio service, which drives the
grip motors as a subwoofer.

**Audio `0x36`.** Four length-tagged sub-packets; the hard parts are the
10.667 ms pacing, Opus CBR 160 kbps, and bit 0 of the enable mask (clear it, or
the controller starts streaming microphone audio back and floods the link).
The subwoofer is 64 × int8 PCM at 6 kHz in the `0x12` block.

**Trigger effects.** Eleven-byte blocks inside the output payload: a mode byte
and ten parameter bytes. `0x05` off, `0x21` feedback, `0x25` weapon, `0x26`
vibration; the official modes address ten trigger *zones* with a 3-bit strength
each (vibration puts its frequency in byte 9). `0x22`/`0x23`/`0x27` (bow,
galloping, machine) are unofficial and firmware-dependent. The panel and the
"buzz while changing volume" path both go through
`Padwave.Output.TriggerEffects`.

**Feature `0x05`.** The calibration words are what make the gyro and
accelerometer meaningful; the kernel turns them into the usual ±2 g / ±2000 °/s
ranges at `/dev/input`.

## Sources

* Linux kernel `drivers/hid/hid-playstation.c` — the structs, report sizes,
  CRC scheme and feature-report offsets used above.
* `LinuxAudio4Dualsense5` — the `0x36` protocol, Opus parameters, pacing and the
  haptic subwoofer; `reverse_engineered/ps5bt_membrane.py` builds the report
  byte for byte.
* `DualSenseClient` — descriptor-derived feature-report ids and lengths, the
  reversed MAC convention, and the USB 48-byte output framing.
* `dualsense-tester` — trigger-effect modes and parameter order, and the
  community field names for the feedback and timestamp bytes.
