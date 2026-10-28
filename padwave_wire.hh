#pragma once
// The wire formats: padwave_number.hh (endian-safe integers) and
// padwave_schemas.hh (the DualSense reports) merged into one header.
// This is the only file that describes the packets.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Similar to HarfBuzz's hb-open-type.hh: wire integer types with implicit
// conversion to arithmetic types, but little-endian instead of HarfBuzz's
// big-endian.
//
// The value is *stored as little-endian bytes*, not as a host integer, so a
// struct made of these types is byte-identical to the wire on any machine —
// big-endian hosts included. get()/set() assemble and split the bytes one at a
// time; there is no memcpy of an integer and no host byte-order assumption.

namespace padwave_number {

template <typename Type, typename UnsignedType = Type>
struct NumType {
  static constexpr unsigned int static_size = sizeof(Type);

  constexpr NumType() = default;

  constexpr explicit NumType(Type value) { set(value); }

  constexpr NumType &operator=(Type o) {
    set(o);
    return *this;
  }

  constexpr operator Type() const { return get(); }

  // bytes[0] is the least significant byte, on every host.
  constexpr Type get() const {
    UnsignedType v = 0;
    for (unsigned int i = 0; i < sizeof(Type); i++)
      v = static_cast<UnsignedType>(((v << 8) | bytes[sizeof(Type) - 1 - i]));
    return static_cast<Type>(v);
  }

  constexpr void set(Type value) {
    UnsignedType v = static_cast<UnsignedType>(value);
    for (unsigned int i = 0; i < sizeof(Type); i++) {
      bytes[i] = static_cast<unsigned char>((v & 0xFF));
      v = static_cast<UnsignedType>((v >> 8));
    }
  }

  constexpr bool operator==(const NumType &o) const { return get() == o.get(); }

  constexpr bool operator!=(const NumType &o) const { return get() != o.get(); }

  constexpr bool operator==(Type o) const { return get() == o; }

  constexpr bool operator!=(Type o) const { return get() != o; }

  constexpr NumType &operator+=(Type n) {
    set(static_cast<Type>((get() + n)));
    return *this;
  }

  constexpr NumType &operator-=(Type n) {
    set(static_cast<Type>((get() - n)));
    return *this;
  }

  constexpr NumType &operator|=(Type n) {
    set(static_cast<Type>((get() | n)));
    return *this;
  }

  constexpr NumType &operator&=(Type n) {
    set(static_cast<Type>((get() & n)));
    return *this;
  }

  constexpr NumType &operator^=(Type n) {
    set(static_cast<Type>((get() ^ n)));
    return *this;
  }

  constexpr NumType &operator++() {
    set(static_cast<Type>((get() + 1)));
    return *this;
  }

  constexpr NumType &operator--() {
    set(static_cast<Type>((get() - 1)));
    return *this;
  }

  unsigned char bytes[sizeof(Type)];
};

} // namespace padwave_number

typedef padwave_number::NumType<uint8_t> PWUINT8;
typedef padwave_number::NumType<uint16_t> PWUINT16;
typedef padwave_number::NumType<uint32_t> PWUINT32;
typedef padwave_number::NumType<int8_t, uint8_t> PWINT8;
typedef padwave_number::NumType<int16_t, uint16_t> PWINT16;
typedef padwave_number::NumType<int32_t, uint32_t> PWINT32;

static_assert(sizeof(PWUINT8) == 1, "PWUINT8 is one byte");
static_assert(sizeof(PWUINT16) == 2, "PWUINT16 is two bytes");
static_assert(sizeof(PWUINT32) == 4, "PWUINT32 is four bytes");
static_assert(sizeof(PWINT8) == 1, "PWINT8 is one byte");
static_assert(sizeof(PWINT16) == 2, "PWINT16 is two bytes");
static_assert(sizeof(PWINT32) == 4, "PWINT32 is four bytes");

namespace padwave {

// Report ordinals, common to every schema. 0x31 is both the input and the
// output state report; the direction is the stream, not the id.
enum Packet : uint8_t {
  PacketInputUsb = 0x01,
  PacketOutputUsb = 0x02,
  PacketFeatureCalibration = 0x05,
  PacketFeaturePairing = 0x09,
  PacketFeatureFirmware = 0x20,
  PacketStateBluetooth = 0x31,
  PacketAudio = 0x36,
};

// The CRC the controller checks: standard CRC-32 (IEEE 802.3, reflected
// polynomial 0xEDB88320), seeded with the stream's seed byte (0xA1 input,
// 0xA2 output, 0xA3 features), over everything before the trailing `crc`
// member. Every report type carries `crc`, `CrcSeed` and a `report_id`, so one
// template covers them all. This is the same result as zlib's crc32(), but
// local so the binary does not need -lz.
static constexpr uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t len) {
  crc = ~crc;
  while (len--) {
    crc ^= *p++;
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

template <typename Report>
struct WireBytes {
  unsigned char data[sizeof(Report)];
};

template <typename Report>
static constexpr uint32_t crc_of(const Report &report) {
  uint8_t seed = static_cast<uint8_t>(Report::CrcSeed);
  uint32_t c = crc32_update(0, &seed, 1);
  auto raw = __builtin_bit_cast(WireBytes<Report>, report);
  return crc32_update(c, raw.data, offsetof(Report, crc));
}

// ===========================================================================
// Output: the 47-byte state body shared by 0x31 (Bluetooth) and 0x02 (USB)
//
// +0x00 in these comments is the offset inside this payload: 0x01 in a USB
// report, 0x03 in a Bluetooth one (after the id, the sequence tag and the 0x10
// tag byte). Only the fields whose valid_flag* bit is set are applied by the
// controller — a claim, not a change notification — which is what makes a
// lightbar-only update safe while the audio stream is running.
// ===========================================================================

struct OutputBody {
  PWUINT8 valid_flag0;              // +0x00  ValidFlag0 bits
  PWUINT8 valid_flag1;              // +0x01  ValidFlag1 bits
  PWUINT8 motor_right;              // +0x02  DS4-compatible rumble
  PWUINT8 motor_left;               // +0x03
  PWUINT8 headphone_volume;         // +0x04  0x00..0x7f
  PWUINT8 speaker_volume;           // +0x05  0x00..0xff
  PWUINT8 mic_volume;               // +0x06  0x00..0x40 (claimed by MicVolume)
  PWUINT8 audio_control;            // +0x07  AudioControl; claimed by AudioRouting
  PWUINT8 mute_button_led;          // +0x08
  PWUINT8 power_save_control;       // +0x09  PowerSave bits
  PWUINT8 right_trigger_mode;       // +0x0a  0x05 off, 0x21 feedback, 0x25 weapon,
                                    //        0x26 vibration (0x22/0x23/0x27 unofficial)
  PWUINT8 right_trigger_params[10]; // +0x0b  zones + 3-bit strength,
                                    //        vibration frequency at byte 9
  PWUINT8 left_trigger_mode;        // +0x15
  PWUINT8 left_trigger_params[10];  // +0x16
  PWUINT8 reserved_32[4];           // +0x20..0x23
  PWUINT8 haptic_volume;            // +0x24
  PWUINT8 audio_control2;           // +0x25  speaker preamp gain
  PWUINT8 valid_flag2;              // +0x26  ValidFlag2 bits
  PWUINT8 reserved_39[2];           // +0x27..0x28
  PWUINT8 lightbar_setup;           // +0x29  valid_flag2 bit 1; 0x02 fades out
  PWUINT8 led_brightness;           // +0x2a
  PWUINT8 player_leds;              // +0x2b  PlayerLeds bits
  PWUINT8 lightbar_red;             // +0x2c
  PWUINT8 lightbar_green;           // +0x2d
  PWUINT8 lightbar_blue;            // +0x2e

  enum : uint8_t { Size = 47 };

  // Which fields this report owns: the kernel's (and DS4Windows', and
  // dualsense-tester's) valid_flag0 / valid_flag1 / valid_flag2. A set bit
  // means "adopt my bytes for this feature"; a clear bit means "leave that
  // feature as it is" — a claim, not a change notification.
  //
  // Example: RightTrigger | LeftTrigger claims only the two trigger blocks,
  // leaving volumes, LEDs and rumble untouched.
  enum ValidFlag0 : uint8_t {
    Rumble = 0x01,          // +0x00 bit 0; main motors also need Haptics
    Haptics = 0x02,         //       bit 1; paired with Rumble for the motors
    RightTrigger = 0x04,    //       bit 2
    LeftTrigger = 0x08,     //       bit 3
    HeadphoneVolume = 0x10, //       bit 4
    SpeakerVolume = 0x20,   //       bit 5
    MicVolume = 0x40,       //       bit 6
    AudioRouting = 0x80,    //       bit 7
  };

  enum ValidFlag1 : uint8_t {
    MuteLed = 0x01,         // +0x01 bit 0
    PowerSave = 0x02,       //       bit 1  enables power_save_control
    Lightbar = 0x04,        //       bit 2
    ResetLights = 0x08,     //       bit 3
    PlayerLeds = 0x10,      //       bit 4
    HapticLpf = 0x20,       //       bit 5
    MotorPowerLevel = 0x40, //       bit 6
    AudioControl2 = 0x80,   //       bit 7
  };

  // +0x26. Only two bits are documented anywhere in the references: bit 0
  // changes the LED brightness and bit 1 enables the fade-out control. Bits
  // 3-7 are reserved.
  enum ValidFlag2 : uint8_t {
    Brightness = 0x01,     // bit 0
    ColorFade = 0x02,      // bit 1: enables the lightbar_setup fade control
    ImprovedRumble = 0x04, // bit 2
  };

  // +0x09. Bit layout: bits 0-3 reserved, bit 4 microphone mute, bit 5
  // reserved, bit 6 audio mute, bit 7 reserved. Only the two documented bits
  // are named (hid-playstation.c uses the first; DS4Windows comments both);
  // the other six are in the panel's power save row so they can be probed: if
  // one stops the motion sensors, the gyro and accelerometer in the UI go flat.
  enum PowerSave : uint8_t {
    MicrophoneMute = 0x10, // bit 4  documented
    AudioMute = 0x40,      // bit 6  documented
  };

  // +0x2b: the five player LEDs, the same pattern the charging sweep animates.
  // Bits 5-7 are reserved.
  enum PlayerLeds : uint8_t {
    Led1 = 0x01, // bit 0
    Led2 = 0x02, // bit 1
    Led3 = 0x04, // bit 2
    Led4 = 0x08, // bit 3
    Led5 = 0x10, // bit 4
  };

  // +0x07: the output path. 0x09 is the membrane speaker, 0x00 the headphone
  // jack; the audio service patched the snapshot to 0x00 when the jack is in
  // use, which is why a silent jack was the first symptom of getting this
  // wrong.
  enum AudioControl : uint8_t {
    Headphones = 0x00,
    Speaker = 0x09,
  };

  // Claims only what the audio path writes: the volumes and the routing.
  // Claiming anything else here is how one writer silently owns a feature
  // another writer is using.
  constexpr void init(AudioControl routing = Headphones) {
    *this = OutputBody{};
    valid_flag0 = HeadphoneVolume | SpeakerVolume | MicVolume | AudioRouting;
    valid_flag1 = AudioControl2;
    headphone_volume = 0x7F;
    speaker_volume = 0x64;
    audio_control = routing;
    audio_control2 = 0x0A;
  }
};

static_assert(sizeof(OutputBody) == OutputBody::Size, "output body must stay 47 bytes");

// ===========================================================================
// Output 0x31 (Bluetooth, 78 bytes, CRC at the end)
// ===========================================================================

struct OutputReport {
  PWUINT8 report_id;     // 0x00  PacketStateBluetooth
  PWUINT8 seq_tag;       // 0x01  high nibble = counter 0..15, low nibble = 0
  PWUINT8 tag;           // 0x02  must be 0x10
  OutputBody body;       // 0x03..0x31
  PWUINT8 reserved4[24]; // 0x32..0x49
  PWUINT32 crc;          // 0x4a  CRC-32, seed 0xA2, little-endian

  enum : uint8_t {
    ReportId = PacketStateBluetooth,
    Size = 78,
    CrcSeed = 0xA2,
  };

  constexpr void init() {
    *this = OutputReport{};
    report_id = ReportId;
    tag = 0x10;
    body.init();
  }

  constexpr void sign() { crc = crc_of(*this); }

  constexpr bool valid() const { return report_id == ReportId && crc == crc_of(*this); }
};

static_assert(sizeof(OutputReport) == OutputReport::Size, "0x31 must stay 78 bytes");

// Output 0x02 (USB, 63 bytes). No CRC — the kernel signs only the Bluetooth
// transport — and the same 47-byte body; sizes from hid-playstation.c:
// DS_OUTPUT_REPORT_USB_SIZE 63, DS_OUTPUT_REPORT_BT_SIZE 78.
struct UsbOutputReport {
  PWUINT8 report_id;    // 0x00  PacketOutputUsb
  OutputBody body;      // 0x01..0x2f
  PWUINT8 reserved[15]; // 0x30..0x3e

  enum : uint8_t { ReportId = PacketOutputUsb, Size = 63 };
};

static_assert(sizeof(UsbOutputReport) == UsbOutputReport::Size, "0x02 must stay 63 bytes");

// ===========================================================================
// Input: the 63-byte body shared by 0x31 (Bluetooth) and 0x01 (USB)
// ===========================================================================

// The 63-byte input payload shared by USB (0x01) and Bluetooth (0x31).
//
// `+0x00` in these comments is the offset inside this payload: 0x01 in a USB
// report, 0x02 in a Bluetooth one. The kernel parses both with a single
// `struct dualsense_input_report`, pointing at &data[1] (USB) or &data[2]
// (Bluetooth); this struct is the same body, and the two packet wrappers below
// splice it in.
//
// When microphone duplex is on, not every 0x31 frame is a gamepad report: the
// controller interleaves audio frames that keep the report id but put the Opus
// TOC 0xD4 at report byte 3 (payload +0x01, i.e. where ly normally is). Demux
// on that byte before trusting any of these fields; MicFrame is the schema for
// those frames.
//
// Every byte is named, either with what it does or with an honest reserved/
// feedback name, and every multi-byte integer is little-endian. Sizes come from
// hid-playstation.c: DS_INPUT_REPORT_USB_SIZE 64, DS_INPUT_REPORT_BT_SIZE 78.
struct InputBody {
  PWUINT8 lx;                 // +0x00  left stick X, 0..255 (128 = centre)
  PWUINT8 ly;                 // +0x01  left stick Y, 0 = up
  PWUINT8 rx;                 // +0x02  right stick X
  PWUINT8 ry;                 // +0x03  right stick Y
  PWUINT8 l2;                 // +0x04  L2 analogue
  PWUINT8 r2;                 // +0x05  R2 analogue
  PWUINT8 seq;                // +0x06  report sequence number
  PWUINT32 buttons;           // +0x07  Buttons bits (d-pad in bits 0-3)
  PWUINT8 incremental;        // +0x0b  increments every report, wraps at 256
  PWUINT8 reserved_14[3];     // +0x0c  constant (0x84, ?, 0x85) except that the
                              //        middle byte ticks up by one every few
                              //        minutes — a slow counter
  PWINT16 gyro_x;             // +0x0f  angular velocity, little-endian
  PWINT16 gyro_y;             // +0x11
  PWINT16 gyro_z;             // +0x13
  PWINT16 accel_x;            // +0x15  acceleration, ~8192 counts per g
  PWINT16 accel_y;            // +0x17
  PWINT16 accel_z;            // +0x19
  PWUINT32 sensor_time;       // +0x1b  timestamp, 0.33 us units
  PWUINT8 temperature;        // +0x1f  controller temperature, degrees C
  PWUINT8 touch1[4];          // +0x20  contact/id, x low, (x high | y low),
                              //        y high; bit 7 of contact = not touching
  PWUINT8 touch2[4];          // +0x24
  PWUINT8 trigger_feedback_l; // +0x28  L2 adaptive-trigger status
  PWUINT8 trigger_feedback_r; // +0x29
  PWUINT32 host_time;         // +0x2a  community calls it the echoed host
                              //        timestamp; a constant 9 for a whole
                              //        session, so unverified
  PWUINT8 device_feedback[4]; // +0x2e  labelled "device feedback", but the low
                              //        bytes change every report and 0x31
                              //        tracks sensor_time's high byte: it
                              //        behaves like a second device clock
  PWUINT8 reserved_52[2];     // +0x32  second byte also tracks sensor_time
  PWUINT8 status0;            // +0x34  battery (low nibble) + charging (high)
  PWUINT8 status1;            // +0x35  Status1 bits
  PWUINT8 status2;            // +0x36  constant 1 in every capture so far
  PWUINT8 aes_cmac[8];        // +0x37  high entropy in every report, like a
                              //        per-report MAC/nonce; not verified
                              //        against the pairing handshake

  enum : uint8_t { Size = 63 };

  // The d-pad is a 4-bit *hat value*, not four bits. The enum turns it into a
  // name; diagonals are the odd values.
  enum Dpad : uint8_t {
    Up = 0,
    UpRight = 1,
    Right = 2,
    DownRight = 3,
    Down = 4,
    DownLeft = 5,
    Left = 6,
    UpLeft = 7,
    None = 8,
  };

  // Four button bytes merged into one little-endian 32-bit field: bits 0..7
  // are the byte at +0x07, 8..15 at +0x08, and so on. Bit positions:
  //   +0x07  bits 0-3 d-pad hat (Dpad), 4 square, 5 cross, 6 circle, 7 triangle
  //   +0x08  bits 0 l1, 1 r1, 2 l2, 3 r2, 4 create, 5 options, 6 l3, 7 r3
  //   +0x09  bits 0 ps, 1 touchpad, 2 mute, 3 reserved, 4 fn1, 5 fn2,
  //          6 left_paddle, 7 right_paddle
  //   +0x0a  eight bits the kernel does not read; maybe L5/R5
  enum Buttons : uint32_t {
    // +0x07
    ButtonSquare = 1u << 4,
    ButtonCross = 1u << 5,
    ButtonCircle = 1u << 6,
    ButtonTriangle = 1u << 7,
    // +0x08
    L1 = 1u << 8,
    R1 = 1u << 9,
    L2 = 1u << 10,
    R2 = 1u << 11,
    Create = 1u << 12,
    Options = 1u << 13,
    L3 = 1u << 14,
    R3 = 1u << 15,
    // +0x09
    Ps = 1u << 16,
    Touchpad = 1u << 17,
    Mute = 1u << 18,
    // bit 19 reserved
    Fn1 = 1u << 20,         // Edge only
    Fn2 = 1u << 21,         // Edge only
    LeftPaddle = 1u << 22,  // Edge only
    RightPaddle = 1u << 23, // Edge only
    // +0x0a: unread by the kernel; maybe L5/R5
  };

  // +0x35. Bits 2-7 are reserved; bit 2 happens to be set on this pad.
  enum Status1 : uint8_t {
    Headset = 0x01,    // bit 0
    Microphone = 0x02, // bit 1
  };
};

static_assert(sizeof(InputBody) == InputBody::Size, "input body must stay 63 bytes");

// ===========================================================================
// Input 0x31 (Bluetooth, 78 bytes) and 0x01 (USB, 64 bytes)
//
// The Bluetooth report is the USB one plus a sequence tag in front and a
// nine-byte tail plus a CRC behind, so every shared field's USB offset is its
// Bluetooth offset minus one. The tail's first byte is not constant (30
// distinct values in a 150 s capture), the other eight were; what it carries is
// unknown. Sizes come from hid-playstation.c (DS_INPUT_REPORT_USB_SIZE 64,
// DS_INPUT_REPORT_BT_SIZE 78); the CRC covers the report with seed 0xA1.
// ===========================================================================

struct InputReport {
  PWUINT8 report_id;      // 0x00  PacketStateBluetooth
  PWUINT8 seq_tag;        // 0x01  high nibble = sequence tag, low nibble = 0
  InputBody body;         // 0x02..0x40
  PWUINT8 bt_reserved[9]; // 0x41..0x49  the first byte is not constant (30
                          //             distinct values in a 150 s capture), the
                          //             other eight were; unknown
  PWUINT32 crc;           // 0x4a  CRC-32 over the report, seed 0xA1

  enum : uint8_t {
    ReportId = PacketStateBluetooth,
    UsbReportId = PacketInputUsb,
    Size = 78,
    UsbSize = 64,
    CrcSeed = 0xA1, // the input stream signs with 0xA1, output with 0xA2
  };

  constexpr void sign() { crc = crc_of(*this); }

  constexpr bool valid() const { return crc == crc_of(*this); }
};

static_assert(sizeof(InputReport) == InputReport::Size, "input 0x31 must stay 78 bytes");

// ===========================================================================
// Audio 0x36 (Bluetooth, 398 bytes)
// ===========================================================================
//
// The undocumented Bluetooth speaker path. Sony says the speaker is USB-only;
// over Bluetooth the controller speaks a proprietary Opus-in-HID protocol that
// LinuxAudio4Dualsense5 reverse-engineered and streams at 93.75 Hz (one packet
// every 512/48000 s). Layout, verified against reverse_engineered/
// ps5bt_membrane.py and the protocol tables:
//
//   0x000  report id 0x36
//   0x001  sequence tag, high nibble (a counter separate from 0x31)
//   0x002  sub-packet 0x11 | 0x80 — audio configuration, length 7
//   0x004  enable mask: bit 0 = microphone capture (0xFE shut, 0xFF open)
//   0x005  5 × haptic block length (64)
//   0x00a  free-running 8-bit counter
//   0x00b  sub-packet 0x10 | 0x80 — state snapshot, length 63
//   0x04c  sub-packet 0x12 | 0x80 — haptics PCM, length 64
//   0x08e  sub-packet 0x13 | 0x80 — speaker Opus (0x16 = headphone jack), 200
//   0x158  50 bytes of padding
//   0x18a  CRC-32, seed 0xA2, little-endian (the same scheme as 0x31)
//
// Three details are the whole difficulty: the 10.667 ms frame interval (not
// 10 ms), Opus CBR 160 kbps, and enable-mask bit 0, which turns on microphone
// capture and floods the link if left set.
//
// The microphone comes back inside the normal 0x31 input report (marker 0xD4,
// Opus 48 kHz mono, 10 ms) when duplex is enabled; see MicFrame below for the
// full recipe. That is why the state snapshot must not mute the mic when
// capture is wanted.
//
// The state snapshot is a whole output payload: its first 47 bytes are
// byte-for-byte OutputBody followed by sixteen bytes of padding, so this
// stream re-states the controller's output configuration 93 times a second and
// every field it does not own must have its valid bit clear or it silently
// overwrites another writer. The cancelled-trigger bug was exactly this: the
// snapshot claimed both trigger effects (0xFD) with a zeroed block; sending
// 0xF1 (volumes and routing only) fixed it. The lightbar was never affected
// because the high byte (0xE3) does not claim the LED colour bit.

struct AudioReport {
  // The state snapshot is the 0x31 output payload plus sixteen bytes of
  // padding. Note valid_flag0: claiming RightTrigger or LeftTrigger here hands
  // the trigger effect to this stream, which writes zeros to it 93 times a
  // second. The fix for the cancelled trigger effects was clearing exactly
  // those two bits (0xFD -> 0xF1).
  //
  // This nested type has to precede the field that uses it; everything else in
  // the class can follow its fields.
  struct State {
    OutputBody body;     // +0x00..0x2e
    PWUINT8 padding[16]; // +0x2f..0x3e  the rest of the 63-byte block
  };

  PWUINT8 report_id;         // 0x000  PacketAudio
  PWUINT8 seq_tag;           // 0x001  high nibble = counter, low nibble = 0
  PWUINT8 config_tag;        // 0x002  0x11 | 0x80 (more sub-packets follow)
  PWUINT8 config_length;     // 0x003  7
  PWUINT8 enable_mask;       // 0x004  EnableMask bits
  PWUINT8 haptic_lengths[5]; // 0x005..0x009  64 each
  PWUINT8 counter;           // 0x00a  free-running 8-bit counter
  PWUINT8 state_tag;         // 0x00b  0x10 | 0x80
  PWUINT8 state_length;      // 0x00c  63
  State state;               // 0x00d..0x04b
  PWUINT8 haptics_tag;       // 0x04c  0x12 | 0x80
  PWUINT8 haptics_length;    // 0x04d  64
  PWINT8 haptics_pcm[64];    // 0x04e..0x08d  64 x int8 PCM at 6 kHz
  PWUINT8 speaker_tag;       // 0x08e  0x13 = membrane, 0x16 = jack (+0x80)
  PWUINT8 speaker_length;    // 0x08f  200
  PWUINT8 opus[200];         // 0x090..0x157  Opus 48 kHz stereo CBR 160 kbps
  PWUINT8 padding[50];       // 0x158..0x189
  PWUINT32 crc;              // 0x18a..0x18d  seed 0xA2

  enum : uint8_t {
    ReportId = PacketAudio,
    StateSize = 63,
    HapticSize = 64,
    OpusSize = 200,
    CrcSeed = 0xA2,
  };

  // The Opus side of the speaker block: 480 samples per frame at 48 kHz (10 ms,
  // CBR 160 kbps = 200 bytes) fed from 512-sample decoder blocks.
  enum : uint16_t { OpusFrames = 480, InputBlock = 512, Channels = 2 };

  static constexpr size_t Size = 398;

  // +0x004. Bit 0 is the microphone capture request: 0xFE keeps it shut, 0xFF
  // opens it and the input stream then also carries 0xD4 frames.
  enum EnableMask : uint8_t {
    MicrophoneCapture = 0x01,
    SpeakerOnly = 0xFE,
    SpeakerAndMic = 0xFF,
  };

  // +0x08e: 0x13 for the membrane speaker, 0x16 for the headphone jack.
  enum SpeakerTag : uint8_t {
    Speaker = 0x13,
    Jack = 0x16,
  };

  // Fill the constant parts, then set seq, counter and whatever state fields
  // this writer owns.
  constexpr void init(SpeakerTag tag, EnableMask mask = SpeakerOnly) {
    *this = AudioReport{};
    report_id = ReportId;
    config_tag = 0x11 | 0x80;
    config_length = 7;
    enable_mask = mask;
    for (int i = 0; i < 5; i++) haptic_lengths[i] = HapticSize;
    state_tag = 0x10 | 0x80;
    state_length = StateSize;
    haptics_tag = 0x12 | 0x80;
    haptics_length = HapticSize;
    speaker_tag = static_cast<uint8_t>((tag | 0x80));
    speaker_length = OpusSize;
  }

  constexpr void sign() { crc = crc_of(*this); }

  constexpr bool valid() const { return report_id == ReportId && crc == crc_of(*this); }
};

static_assert(sizeof(AudioReport::State) == AudioReport::StateSize,
              "the audio state snapshot must stay 63 bytes");
static_assert(sizeof(AudioReport) == AudioReport::Size, "0x36 must stay 398 bytes");

// ===========================================================================
// Feature reports — the configuration channel
// ===========================================================================
//
// Read (and occasionally written) with a HID GET_REPORT/SET_REPORT on report
// ids that are not part of the streaming protocol. The kernel uses 0x05, 0x09
// and 0x20 and the offsets below come from hid-playstation.c; DualSenseClient's
// descriptor notes corroborate the ids and lengths. Feature reports are signed
// with CRC-32 seed 0xA3 over Bluetooth (the streams use 0xA1 input, 0xA2
// output). padwave does not read them — the kernel already exposes the results
// (MAC, battery, calibration) — but they are here so the whole protocol is
// visible.
//
//   id   length  what
//   0x05 41      motion-sensor calibration: 17 int16 LE at buf[1..34]
//   0x09 20      pairing info; MAC at buf[1..6] (some clients read it reversed)
//   0x20 64      firmware/hardware: hw buf[24], fw buf[28], update buf[44]
//   0x80 —       factory command write (Edge/vendor tooling)
//   0x81 64      the matching command response
//   0x65 64      DualSense Edge factory handshake
//
// The Edge also exposes profile reports in the 0x60..0x7B range, beyond what
// padwave needs.

struct Calibration {
  PWUINT8 report_id; // 0x00  PacketFeatureCalibration

  // 0x01..0x22: seventeen little-endian int16: three gyro biases, then
  // plus/minus at 2x speed for each axis, then gyro speed limits, then accel
  // plus/minus at 1 g per axis.
  PWINT16 gyro_pitch_bias;
  PWINT16 gyro_yaw_bias;
  PWINT16 gyro_roll_bias;
  PWINT16 gyro_pitch_plus;
  PWINT16 gyro_pitch_minus;
  PWINT16 gyro_yaw_plus;
  PWINT16 gyro_yaw_minus;
  PWINT16 gyro_roll_plus;
  PWINT16 gyro_roll_minus;
  PWINT16 gyro_speed_plus;
  PWINT16 gyro_speed_minus;
  PWINT16 acc_x_plus;
  PWINT16 acc_x_minus;
  PWINT16 acc_y_plus;
  PWINT16 acc_y_minus;
  PWINT16 acc_z_plus;
  PWINT16 acc_z_minus;
  PWUINT8 unknown_35[6]; // 0x23..0x28  not used by the kernel

  enum : uint8_t { ReportId = PacketFeatureCalibration, Size = 41 };
};

static_assert(sizeof(Calibration) == Calibration::Size, "0x05 must stay 41 bytes");

struct Pairing {
  PWUINT8 report_id;      // 0x00  PacketFeaturePairing
  PWUINT8 mac[6];         // 0x01..0x06  the controller's own address
  PWUINT8 unknown_07[13]; // 0x07..0x13

  enum : uint8_t { ReportId = PacketFeaturePairing, Size = 20 };
};

static_assert(sizeof(Pairing) == Pairing::Size, "0x09 must stay 20 bytes");

struct Firmware {
  PWUINT8 report_id;       // 0x00  PacketFeatureFirmware
  PWUINT8 unknown_01[23];  // 0x01..0x17
  PWUINT32 hw_version;     // 0x18..0x1b
  PWUINT32 fw_version;     // 0x1c..0x1f
  PWUINT8 unknown_32[12];  // 0x20..0x2b
  PWUINT16 update_version; // 0x2c..0x2d  feature/PCB revision, varies by model
  PWUINT8 unknown_46[18];  // 0x2e..0x3f

  enum : uint8_t { ReportId = PacketFeatureFirmware, Size = 64 };
};

static_assert(sizeof(Firmware) == Firmware::Size, "0x20 must stay 64 bytes");

// ===========================================================================
// Microphone frames inside the 0x31 input stream
// ===========================================================================
//
// The controller has no audio class over Bluetooth: the microphone is
// multiplexed into the same HID reports as the gamepad, and its audio is Opus,
// not PCM. Byte 3 is the constant Opus TOC 0xD4; a state report has the
// sequence/tag nibbles there instead.
//
//   0x31 seq .. 0xD4 | one Opus frame | CRC-32 (4 bytes, seed 0xA1)
//
// Opus, 48 kHz, mono, 480 samples (10 ms), about 100 packets per second.
//
// Turning capture on takes three writes in the outgoing 0x36:
//   1. power_save_control (+0x09) bit 4 = 0 AND valid_flag1 bit 1 claims the
//      byte (without the claim the unmute is inert);
//   2. enable mask (0x36 byte 4) bit 0 set: 0xFF instead of 0xFE;
//   3. mic_volume (+0x06) = 0x40, claimed by valid_flag0 bit 6.
// Step 2 is the tap; step 1 is the one people forget.
//
// It disturbs the gamepad input: these frames carry the same report id as the
// state report, so anything that assumes every 0x31 is 78 bytes reads Opus
// bytes as sticks and buttons. The kernel ignores them, so there is no ALSA
// capture device; decoding them is a host-side job. Duplex roughly doubles the
// link traffic (speaker down and mic up are each ~160 kbps of Opus).

struct MicFrame {
  enum : uint8_t {
    ReportId = PacketStateBluetooth,
    Toc = 0xD4,
    CrcBytes = 4,
  };

  enum : uint32_t {
    SampleRate = 48000,
    Samples = 480,
    FrameMs = 10,
  };

  static bool is_frame(const uint8_t *buf, size_t len) {
    return len > 3 + CrcBytes && buf[0] == ReportId && buf[3] == Toc;
  }

  // The Opus packet: everything after the TOC byte, minus the trailing CRC.
  static const uint8_t *payload(const uint8_t *buf, size_t len, size_t *out_len) {
    if (!is_frame(buf, len)) return nullptr;
    *out_len = len - 3 - CrcBytes;
    return buf + 3;
  }
};

} // namespace padwave

// ===========================================================================
// Compile-time wire checks
// ===========================================================================
//
// These static_asserts are part of the header, so every translation unit that
// uses the schema validates endianness, offsets, initialized fields and CRC.
namespace wire_test {

// --- byte order ------------------------------------------------------------

constexpr PWUINT16 kU16 = [] {
  PWUINT16 v;
  v = 0x1234;
  return v;
}();
static_assert(kU16.bytes[0] == 0x34 && kU16.bytes[1] == 0x12);
static_assert(kU16.get() == 0x1234);

constexpr PWUINT32 kU32 = [] {
  PWUINT32 v;
  v = 0x01020304;
  return v;
}();
static_assert(kU32.bytes[0] == 0x04 && kU32.bytes[1] == 0x03 && kU32.bytes[2] == 0x02 &&
              kU32.bytes[3] == 0x01);
static_assert(kU32.get() == 0x01020304);

constexpr PWINT16 kS16 = [] {
  PWINT16 v;
  v = -2;
  return v;
}();
static_assert(kS16.bytes[0] == 0xFE && kS16.bytes[1] == 0xFF);
static_assert(kS16.get() == -2);

constexpr PWINT32 kS32 = [] {
  PWINT32 v;
  v = -1;
  return v;
}();
static_assert(kS32.bytes[0] == 0xFF && kS32.bytes[3] == 0xFF);

// --- CRC-32 ----------------------------------------------------------------

constexpr uint8_t kCheck[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
static_assert(padwave::crc32_update(0, kCheck, sizeof(kCheck)) == 0xCBF43926u);
constexpr uint32_t kChained = [] {
  uint32_t c = padwave::crc32_update(0, kCheck, 4);
  return padwave::crc32_update(c, kCheck + 4, 5);
}();
static_assert(kChained == 0xCBF43926u);

// --- initialized reports and offsets --------------------------------------

constexpr padwave::AudioReport kReport = [] {
  padwave::AudioReport r{};
  r.init(padwave::AudioReport::Jack, padwave::AudioReport::SpeakerOnly);
  r.state.body.init(padwave::OutputBody::Headphones);
  r.seq_tag = 0x00;
  r.counter = 0;
  return r;
}();

static_assert(sizeof(padwave::AudioReport) == 398);
static_assert(sizeof(padwave::AudioReport::State) == 63);
static_assert(sizeof(padwave::OutputBody) == 47);
static_assert(offsetof(padwave::AudioReport, report_id) == 0);
static_assert(offsetof(padwave::AudioReport, seq_tag) == 1);
static_assert(offsetof(padwave::AudioReport, config_tag) == 2);
static_assert(offsetof(padwave::AudioReport, state) == 13);
static_assert(offsetof(padwave::AudioReport, speaker_tag) == 142);
static_assert(offsetof(padwave::AudioReport, opus) == 144);
static_assert(offsetof(padwave::AudioReport, crc) == 394);
static_assert(offsetof(padwave::OutputBody, valid_flag0) == 0);
static_assert(offsetof(padwave::OutputBody, headphone_volume) == 4);
static_assert(offsetof(padwave::OutputBody, lightbar_setup) == 0x29);

static_assert(kReport.report_id.get() == 0x36);
static_assert(kReport.seq_tag.get() == 0x00);
static_assert(kReport.config_tag.get() == 0x91);
static_assert(kReport.config_length.get() == 7);
static_assert(kReport.enable_mask.get() == padwave::AudioReport::SpeakerOnly);
static_assert(kReport.haptic_lengths[0].get() == 64);
static_assert(kReport.haptic_lengths[4].get() == 64);
static_assert(kReport.counter.get() == 0);
static_assert(kReport.state_tag.get() == 0x90);
static_assert(kReport.state_length.get() == 63);
static_assert(kReport.state.body.valid_flag0.get() == 0xF0);
static_assert(kReport.state.body.valid_flag1.get() == 0x80);
static_assert(kReport.state.body.headphone_volume.get() == 0x7F);
static_assert(kReport.state.body.speaker_volume.get() == 0x64);
static_assert(kReport.speaker_tag.get() == (padwave::AudioReport::Jack | 0x80));
static_assert(kReport.speaker_length.get() == 200);
static_assert(kReport.opus[0].get() == 0);

// --- sign / validate / byte copy, all constexpr ---------------------------

constexpr padwave::AudioReport kSigned = [] {
  padwave::AudioReport r = kReport;
  r.sign();
  return r;
}();
static_assert(kSigned.valid());

constexpr auto kRaw = __builtin_bit_cast(padwave::WireBytes<padwave::AudioReport>, kSigned);
constexpr padwave::AudioReport kCopy = __builtin_bit_cast(padwave::AudioReport, kRaw);
static_assert(kCopy.valid());

} // namespace wire_test
