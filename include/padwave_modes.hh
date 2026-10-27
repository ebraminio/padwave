#pragma once
#include <stddef.h>
#include <stdint.h>
// Entry points and in-process engines. The service calls the player/input
// engines directly from its epoll loop; the *_main wrappers keep the debug
// modes working.

int native_main(int argc, char **argv);
int input_main(int argc, char **argv);
int opusdec_main(int argc, char **argv);
int server_main(int argc, char **argv);

void *player_create(const char *node, bool speaker);
void player_set_sink(void *handle, void (*sink)(const char *, void *), void *ud);
int player_timer_fd(void *handle);
int player_bus_fd(void *handle);
void player_step(void *handle);
void player_on_bus(void *handle);
void player_command(void *handle, const char *line);
void player_destroy(void *handle);

void *input_create(const char *node);
void input_set_sink(void *handle, void (*sink)(const char *, void *), void *ud);
int input_fd(void *handle);
int input_feed(void *handle, const uint8_t *buf, size_t len);
void input_destroy(void *handle);
