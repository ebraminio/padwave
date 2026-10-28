// Compile-time checks for the wire layout, endianness and CRC.
//
// A big-endian host runs the same static_asserts, because the byte order lives
// in NumType rather than in host integers.
#include <bit>
#include <stddef.h>

#include "padwave_wire.hh"

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

constexpr auto kRaw = std::bit_cast<padwave::WireBytes<padwave::AudioReport>>(kSigned);
constexpr padwave::AudioReport kCopy = std::bit_cast<padwave::AudioReport>(kRaw);
static_assert(kCopy.valid());
