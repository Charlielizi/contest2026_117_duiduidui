/****************************************************************************
 * Cross-process ownership guard for the shared Wi-Fi radio
 ****************************************************************************/

#pragma once

int wifi_radio_guard_acquire(void);
int wifi_radio_guard_try_acquire(void);
void wifi_radio_guard_release(void);
