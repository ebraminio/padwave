# DualSense protocol catalogue

What is known about the controller's report ids, what padwave does with each,
and where the knowledge comes from. The descriptions themselves live in
`padwave_wire.hh` and summarised at `GET /api/schema`.

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
| `0x31` | in | variable | microphone Opus: same report id, but byte 3 is the TOC `0xD4` | not decoded; see [Microphone stream](#microphone-stream) |
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
updates coexist with the audio service's stream. The main grip motors are
`valid_flag0` bit 0 (`rumble`) **plus** bit 1 (`haptics`); DS4Windows and
dualsense-tester both set the pair, and bit 0 alone is ignored. Padwave's lab
claims both for the start and the zero stop.

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

## Microphone stream

The DualSense has no audio class over Bluetooth: the microphone shares the
reports with everything else, and its frames are Opus, not PCM. The schema and
the demultiplexing helpers live in `Padwave.Schema.DualSenseMic`; the short
version follows.

Capturing it takes three separate changes; all three live in the outgoing `0x36`
audio report.

### Enabling it

| # | field | value | why |
|---|---|---|---|
| 1 | `power_save_control` (+0x09) | bit 4 = 0, **and** `valid_flag1` bit 1 (`power_save`) claims the byte | unmutes the mic — without the claim the write is inert |
| 2 | audio enable mask (`0x36` byte 4) | bit 0 set: `0xFF` instead of `0xFE` | asks the controller to start sending capture frames |
| 3 | `mic_volume` (+0x06) | `0x40`, claimed by `valid_flag0` bit 6 | without it the frames are silence |

Step 2 is the one that opens the tap; step 1 is the one people forget, because
the controller happily accepts an unmute for a field nobody claimed.

### What comes back

Still report id `0x31`, but **not** the state report: byte 3 is the constant
Opus table-of-contents byte `0xD4`. That marker is the whole demultiplexing
trick — a state report has the sequence/tag nibbles there instead.

```
0x31  seq  ..  0xD4 | one Opus frame | CRC-32 (4 bytes, seed 0xA1)
```

Frames arrive about every 10 ms. The audio project's reader is exactly this
filter plus a decoder:

```c
if (buf[0] != 0x31 || n < 16 || buf[3] != MIC_TOC) continue;
int ns = opus_decode(dec, buf + 3, n - 3 - 4, pcm, MIC_FRAME, 0);
```

### Format

| property | value |
|---|---|
| codec | Opus (not PCM) |
| sample rate | 48 000 Hz |
| channels | mono |
| frame | 480 samples = 10 ms |
| packet rate | ~100/s |
| trailing bytes | CRC-32, seed `0xA1` |

`LinuxAudio4Dualsense5` decodes each frame to s16, stores it in a one-second
ring buffer with a 100 ms prebuffer, and publishes it as the PipeWire source
`ds5_mic` — that node is the only way to hear it, because the kernel
does not decode these blocks at all.

### Caveats

* **It disturbs the gamepad input.** Mic frames carry the same report id as the
  state report; anything that assumes every `0x31` is 78 bytes will read them as
  wild sticks and buttons. Padwave drops them by length, but they still cost
  link time and can delay state frames — the audio project says so next to its
  own microphone checkbox.
* **The kernel ignores it.** `hid-playstation` never parses the audio blocks, so
  there is no ALSA capture device; `ds5_mic` belongs to the audio project.
* **Both directions share the link.** Speaker down and mic up are each roughly
  160 kbps of Opus; duplex nearly doubles the Bluetooth traffic.
* **Our own sink no longer claims `power_save`.** The membrane snapshot was
  trimmed to audio-output fields only, so if the microphone is ever enabled
  there, bit 1 of `valid_flag1` has to come back or step 1 does nothing.

## Two writers, one state

**Two writers, one state.** The `0x36` audio stream carries a state snapshot
(sub-packet `0x10`) every 10.667 ms, and its `valid_flag0` decides which fields
*that* stream owns. If it claims the trigger bits (LinuxAudio4Dualsense5's
snapshot used `0xFD`, i.e. both trigger flags set, with a zeroed trigger block),
it silently cancels any adaptive-trigger effect another writer applied — the
effect works for a fraction of a frame and is then reset. Clearing bits 2 and 3
(`0xFD` → `0xF1`) leaves volumes and routing claimed and the triggers alone.
The same rule applies to any field two writers might touch: never claim a field
you do not intend to write.

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
