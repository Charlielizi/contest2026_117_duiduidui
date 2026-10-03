/* SPDX-License-Identifier: Apache-2.0 */
#include "core/agent_start_gate.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool reserved;
int agent_start_gate_acquire(int argc, char *argv[])
{
    if (argc != 1) {
        bool help = argc == 2 && argv && argv[1] &&
            (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"));
        puts("Usage: ai_agent [--help]\nWithout arguments starts the service once; help does not start any services.");
        return help ? 1 : -EINVAL;
    }
    pthread_mutex_lock(&lock);
    int result = reserved ? -EBUSY : 0;
    if (!result) reserved = true;
    pthread_mutex_unlock(&lock);
    if (result) puts("AI Agent is already started (or needs reboot after a failed initialization).");
    return result;
}
void agent_start_gate_release(void)
{
    pthread_mutex_lock(&lock); reserved = false; pthread_mutex_unlock(&lock);
}
