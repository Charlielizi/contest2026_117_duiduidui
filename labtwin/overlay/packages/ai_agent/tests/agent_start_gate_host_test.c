/* SPDX-License-Identifier: Apache-2.0 */
#include "core/agent_start_gate.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
static int starts;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *try_start(void *unused)
{
    (void)unused;
    char *argv[] = { "ai_agent", NULL };
    int result = agent_start_gate_acquire(1, argv);
    if (!result) { pthread_mutex_lock(&lock); starts++; pthread_mutex_unlock(&lock); }
    else assert(result == -EBUSY);
    return NULL;
}
int main(void)
{
    char *help[] = { "ai_agent", "help", NULL };
    char *bad[] = { "ai_agent", "bad", NULL };
    assert(agent_start_gate_acquire(2, help) == 1);
    assert(agent_start_gate_acquire(2, bad) == -EINVAL);
    assert(agent_start_gate_acquire(0, NULL) == -EINVAL);
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, try_start, NULL));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(starts == 1); assert(agent_start_gate_acquire(2, help) == 1);
    agent_start_gate_release(); try_start(NULL); assert(starts == 2);
    puts("PASS start gate: help/invalid arguments are inert; concurrent launches reserve once");
}
