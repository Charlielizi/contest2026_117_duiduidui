/* Short-lived, non-interactive feedback overlay owned by the LVGL thread. */
#pragma once

#include <stdint.h>

int agent_feedback_display_show(const char *text, uint32_t duration_seconds);
