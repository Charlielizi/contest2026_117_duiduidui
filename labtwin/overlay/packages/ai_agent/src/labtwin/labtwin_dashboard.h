/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define LABTWIN_DASHBOARD_PROTOCOL "labtwin.dashboard.v1"
#define LABTWIN_DASHBOARD_PATH "/labtwin"

typedef struct {
    bool claimed;
    bool authenticated;
} labtwin_dashboard_session_t;

void labtwin_dashboard_session_init(labtwin_dashboard_session_t *session,
                                    bool claimed);
bool labtwin_dashboard_handle(labtwin_dashboard_session_t *session,
                              const char *message, size_t message_size,
                              char **response);

/* Build the same canonical snapshot payload used by the WebSocket sync op. */
int labtwin_dashboard_snapshot_json(char *out, size_t out_size);
