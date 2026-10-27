#pragma once
// The standalone app behind `padwave serve`: stations, the player child, the
// input child and the state the panel reads.
#include <stddef.h>

bool app_init();                       // scan stations, spawn children
void app_shutdown();
void app_tick();                       // called by the server timer
int app_fds(int *fds, int max);        // child stdout fds for epoll
void app_handle_fd(int fd);            // one readable child fd
int app_status_json(char *buf, size_t cap);
int app_library_json(char *buf, size_t cap);
int app_config_json(char *buf, size_t cap);
int app_bindings_json(char *buf, size_t cap);
int app_command(const char *action, const char *body, size_t body_len, char *out, size_t out_cap);
int app_handle_command(const char *body, size_t len, char *out, size_t cap);
bool app_owns_fd(int fd);
