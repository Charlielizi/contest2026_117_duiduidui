#pragma once

#include <stdbool.h>

int labtwin_environment_init(void);
void labtwin_environment_submit(bool temperature_valid, float temperature_c,
                                bool humidity_valid, float humidity_percent);

#ifdef LABTWIN_ENV_HOST_TEST
void labtwin_environment_process_test(unsigned long long now_ms);
void labtwin_environment_telemetry_sample_test(bool temperature_valid,
                                               float temperature_c,
                                               bool humidity_valid,
                                               float humidity_percent,
                                               unsigned long long now_ms);
void labtwin_environment_shutdown_test(void);
#endif
