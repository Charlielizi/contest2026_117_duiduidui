/*
 * Copyright (C) 2026 Xiaomi Corporation
 * Licensed under the Apache License, Version 2.0
 */

#include "ui/agent_feedback_display.h"
#include "agent_compat.h"

#include <errno.h>

int agent_feedback_display_show(const char *text, uint32_t duration_seconds)
{
#if defined(CONFIG_LUNCHER_MINI_APP)
    extern int launcher_board_feedback_post(const char *text,
                                            uint32_t duration_seconds);
    return launcher_board_feedback_post(text, duration_seconds);
#else
    (void)text;
    (void)duration_seconds;
    return -ENOSYS;
#endif
}
