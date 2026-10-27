// Checks that the wire layout is what the controller expects, and that the
// integers are little-endian by construction rather than by host. Runs on any
// machine: a big-endian box would execute exactly the same assertions.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "padwave_wire.hh"

int main() {
  // The byte order belongs to the type, not the host.
  PWUINT16 u16;
  u16 = 0x1234;
  assert(u16.bytes[0] == 0x34 && u16.bytes[1] == 0x12);

  PWUINT32 u32;
  u32 = 0x01020304;
  assert(u32.bytes[0] == 0x04 && u32.bytes[1] == 0x03 && u32.bytes[2] == 0x02 &&
         u32.bytes[3] == 0x01);

  PWINT16 s16;
  s16 = -2;
  assert(s16.bytes[0] == 0xFE && s16.bytes[1] == 0xFF);

  PWINT32 s32;
  s32 = -1;
  assert(s32.bytes[0] == 0xFF && s32.bytes[3] == 0xFF);

  assert(u32.get() == 0x01020304);
  assert(s16.get() == -2);

  // A whole report: the header, the audio state snapshot and the CRC.
  padwave::AudioReport report;
  report.init(padwave::AudioReport::Jack, padwave::AudioReport::SpeakerOnly);
  report.state.body.init(padwave::OutputBody::Headphones);
  report.seq_tag = 0x00;
  report.counter = 0;
  report.sign();

  const unsigned char *b = (const unsigned char *)&report;
  assert(sizeof(report) == 398);
  assert(b[0] == 0x36);              // report id
  assert(b[1] == 0x00);              // seq tag
  assert(b[2] == 0x91 && b[3] == 7); // config sub-packet
  assert(b[4] == 0xFE);              // speaker only, mic shut
  assert(b[5] == 64 && b[9] == 64);  // haptic lengths
  assert(b[10] == 0);                // counter
  assert(b[11] == 0x90 && b[12] == 63);
  assert(b[13] == 0xF0 && b[14] == 0x80);           // valid_flag0/1 of the snapshot
  assert(b[17] == 0x7F && b[18] == 0x64);           // headphone/speaker volume (state +0x04/+0x05)
  assert(b[142] == (0x16 | 0x80) && b[143] == 200); // jack + Opus block
  assert(report.valid());

  // And it survives a memcpy, which is how it is written.
  padwave::AudioReport copy;
  memcpy(&copy, b, sizeof(copy));
  assert(copy.valid());
  assert(copy.opus[0].get() == 0);

  printf("wire layout ok: %zu bytes, little-endian by construction, crc valid\n", sizeof(report));
  return 0;
}
