// padwave — one binary, two modes.
//
// Every piece of the audio path lives behind this entry point: the player that
// owns decode, resample, Opus and the 0x36 report, and the small codec helper
// the Elixir side uses for the microphone decoder and the lab's encoder. They
// share scripts/padwave_schemas.hh, so the protocol is described once.
#include <string.h>

#include "padwave_modes.hh"

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "opusdec") == 0) return opusdec_main(argc - 1, argv + 1);
  return native_main(argc - 1, argv + 1);
}
