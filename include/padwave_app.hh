#pragma once
// Opaque handle to the standalone app. The state and methods live in
// padwave_app.cc; the server owns one instance and passes it to the callbacks
// as userdata.
#include <stddef.h>

struct app;

struct app *app_create();
void app_destroy(struct app *a);
void app_tick(struct app *a);
int app_fds(struct app *a, int *fds, int max);
void app_handle_fd(struct app *a, int fd);
int app_status_json(struct app *a, char *buf, size_t cap);
int app_library_json(struct app *a, char *buf, size_t cap);
int app_config_json(char *buf, size_t cap);
int app_bindings_json(char *buf, size_t cap);
int app_command(struct app *a, const char *action, const char *body, size_t body_len,
                char *out, size_t out_cap);
int app_handle_command(struct app *a, const char *body, size_t len, char *out, size_t cap);
bool app_owns_fd(struct app *a, int fd);
