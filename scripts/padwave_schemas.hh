#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>

#include "padwave_number.hh"

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

// The CRC the controller checks: zlib's crc32 seeded with the stream's seed
// byte (0xA1 input, 0xA2 output, 0xA3 features), over everything before the
// trailing `crc` member. Every report type carries `crc`, `CrcSeed` and a
// `report_id`, so one template covers them all.
template <typename Report>
static inline uint32_t crc_of(const Report &report) {
  uint8_t seed = (uint8_t)Report::CrcSeed;
  uLong c = crc32(0L, &seed, 1);
  return (uint32_t)crc32(c, (const Bytef *)&report, offsetof(Report, crc));
}

// ===========================================================================
// Output: the 47-byte state body shared by 0x31 (Bluetooth) and 0x02 (USB)
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
  PWUINT8 lightbar_setup;           // +0x29  fade-out after power-on
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
    Rumble = 0x01,          // +0x00 bit 0
    Haptics = 0x02,         //       bit 1
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
  // changes the LED brightness and bit 1 enables the fade-out control.
  enum ValidFlag2 : uint8_t {
    Brightness = 0x01,     // bit 0
    ColorFade = 0x02,      // bit 1
    ImprovedRumble = 0x04, // bit 2
  };

  // +0x09. Only two bits are documented anywhere in the references: bit 4
  // mutes the microphone and bit 6 mutes audio (hid-playstation.c uses the
  // first; DS4Windows comments both). The other six are in the panel's power
  // save row so they can be probed: if one stops the motion sensors, the gyro
  // and accelerometer in the UI go flat.
  enum PowerSave : uint8_t {
    MicrophoneMute = 0x10, // bit 4  documented
    AudioMute = 0x40,      // bit 6  documented
  };

  // +0x2b: the five player LEDs, the same pattern the charging sweep animates.
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
  void init(AudioControl routing = Headphones) {
    memset(this, 0, sizeof(*this));
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

  void init() {
    memset(this, 0, sizeof(*this));
    report_id = ReportId;
    tag = 0x10;
    body.init();
  }

  void sign() { crc = crc_of(*this); }

  bool valid() const { return report_id == ReportId && crc == crc_of(*this); }
};

static_assert(sizeof(OutputReport) == OutputReport::Size, "0x31 must stay 78 bytes");

// ===========================================================================
// Input: the 63-byte body shared by 0x31 (Bluetooth) and 0x01 (USB)
// ===========================================================================

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
  PWUINT8 touch1[4];          // +0x20  contact/id, x low, (x high | y low), y high
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
  // are the byte at +0x07, 8..15 at +0x08, and so on.
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
    Fn1 = 1u << 20,         // Edge only
    Fn2 = 1u << 21,         // Edge only
    LeftPaddle = 1u << 22,  // Edge only
    RightPaddle = 1u << 23, // Edge only
    // +0x0a: unread by the kernel; maybe L5/R5
  };

  // +0x35. Bit 7 of a touch contact byte means "not touching", which is why
  // the touch fields above are left as raw bytes here.
  enum Status1 : uint8_t {
    Headset = 0x01,    // bit 0
    Microphone = 0x02, // bit 1
  };
};

static_assert(sizeof(InputBody) == InputBody::Size, "input body must stay 63 bytes");

// ===========================================================================
// Input 0x31 (Bluetooth, 78 bytes) and 0x01 (USB, 64 bytes)
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

  bool valid() const { return crc == crc_of(*this); }
};

static_assert(sizeof(InputReport) == InputReport::Size, "input 0x31 must stay 78 bytes");

// ===========================================================================
// Audio 0x36 (Bluetooth, 398 bytes)
// ===========================================================================

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
  void init(SpeakerTag tag, EnableMask mask = SpeakerOnly) {
    memset(this, 0, sizeof(*this));
    report_id = ReportId;
    config_tag = 0x11 | 0x80;
    config_length = 7;
    enable_mask = mask;
    for (int i = 0; i < 5; i++) haptic_lengths[i] = HapticSize;
    state_tag = 0x10 | 0x80;
    state_length = StateSize;
    haptics_tag = 0x12 | 0x80;
    haptics_length = HapticSize;
    speaker_tag = (uint8_t)(tag | 0x80);
    speaker_length = OpusSize;
  }

  void sign() { crc = crc_of(*this); }

  bool valid() const { return report_id == ReportId && crc == crc_of(*this); }
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
// with CRC-32 seed 0xA3 over Bluetooth. padwave does not read them — the kernel
// already exposes the results — but they are here so the whole protocol is
// visible.

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
// The DualSense has no audio class over Bluetooth: the microphone is
// multiplexed into the same HID reports as the gamepad, and its audio is Opus,
// not PCM. Byte 3 is the constant Opus TOC 0xD4; a state report has the
// sequence/tag nibbles there instead.
//
//   0x31 seq .. 0xD4 | one Opus frame | CRC-32 (4 bytes, seed 0xA1)
//
// Opus, 48 kHz, mono, 480 samples (10 ms), about 100 packets per second.
//
// It disturbs the gamepad input: these frames carry the same report id as the
// state report, so anything that assumes every 0x31 is 78 bytes reads Opus
// bytes as sticks and buttons. The kernel ignores them, so there is no ALSA
// capture device; decoding them is a host-side job.

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
