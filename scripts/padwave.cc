// padwave — one binary, the native modes.
//
//   padwave native            the player: decode, Opus and 0x36/0x31
//   padwave input             the input decoder: 0x31 -> state lines
//   padwave opusdec [-e|-s]   the codec helper
//
// All of them include scripts/padwave_schemas.hh, so the protocol is described
// once and no mode redefines a packet.
#include <string.h>

#include "padwave_modes.hh"

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "opusdec") == 0) return opusdec_main(argc - 1, argv + 1);
  if (argc > 1 && strcmp(argv[1], "input") == 0) return input_main(argc - 1, argv + 1);
  return native_main(argc - 1, argv + 1);
}
