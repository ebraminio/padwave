#pragma once
// The two modes of the single padwave binary, so the orchestrating main() has
// nothing but a dispatch table.
//
//   padwave native            the player (default): decode, Opus, 0x36
//   padwave opusdec [-e|-s]   the codec helper: encode or decode framed Opus
int native_main(int argc, char **argv);
int native_run(int in_fd, int out_fd, const char *node, bool speaker);
int input_run(const char *node, int out_fd, int stop_fd);
int opusdec_main(int argc, char **argv);
int input_main(int argc, char **argv);
int server_main(int argc, char **argv);
