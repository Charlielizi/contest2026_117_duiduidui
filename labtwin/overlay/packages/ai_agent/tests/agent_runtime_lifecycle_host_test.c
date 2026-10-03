#include "agent_config.h"
#include "core/message_bus.h"
#include "core/session_mgr.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int result;
} pop_result_t;

static void *wait_for_inbound(void *arg)
{
    agent_msg_t message = { 0 };
    pop_result_t *result = arg;

    result->result = message_bus_pop_inbound(&message, UINT32_MAX);
    if (result->result == OK) {
        message_bus_msg_free(&message);
    }
    return NULL;
}

static int line_count(const char *path)
{
    char line[256];
    int count = 0;
    FILE *file = fopen(path, "r");

    assert(file != NULL);
    while (fgets(line, sizeof(line), file) != NULL) {
        count++;
    }
    assert(fclose(file) == 0);
    return count;
}

static void ensure_dir(const char *path)
{
    assert(mkdir(path, 0700) == 0 || errno == EEXIST);
}

int main(void)
{
    char history[4096];
    char content[64];
    char path[256];
    pthread_t waiter;
    pop_result_t result = { 0 };

    ensure_dir(AGENT_DATA_DIR);
    ensure_dir(AGENT_SESSION_DIR);
    assert(session_mgr_init() == OK);

    for (int i = 0; i < AGENT_SESSION_MAX_MSGS * 2 + 10; i++) {
        snprintf(content, sizeof(content), "message-%02d", i);
        assert(session_append("room/alpha", "user", content) == OK);
    }

    snprintf(path, sizeof(path), "%s/tg_room_alpha.jsonl",
        AGENT_SESSION_DIR);
    assert(line_count(path) == AGENT_SESSION_MAX_MSGS * 2);
    assert(session_get_history_json("room/alpha", history, sizeof(history),
        AGENT_AI_AGENT_MAX_HISTORY) == OK);
    assert(strstr(history, content) != NULL);
    assert(session_get_history_json("room/alpha", history, sizeof(history),
        0) == ERROR);
    assert(strcmp(history, "[]") == 0);

    assert(message_bus_init() == OK);
    assert(pthread_create(&waiter, NULL, wait_for_inbound, &result) == 0);
    usleep(10 * 1000);
    message_bus_wakeup();
    assert(pthread_join(waiter, NULL) == 0);
    assert(result.result == ERROR);

    agent_msg_t message = { .content = strdup("shutdown") };
    assert(message.content != NULL);
    assert(message_bus_push_inbound(&message) == ERROR);
    message_bus_msg_free(&message);
    message_bus_destroy();

    assert(session_clear_all() == OK);
    puts("agent_runtime_lifecycle_host_test: PASS");
    return 0;
}
