#pragma once

#include <stdbool.h>

/* Serve the immutable LabTwin SPA from /resource/labtwin-admin. */
bool web_static_try_handle(int fd, const char *request, int request_len);
