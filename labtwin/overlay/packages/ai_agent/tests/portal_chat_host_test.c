#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static uint64_t now;
static uint64_t fake_now(void) { return now; }
#define PORTAL_CHAT_NOW fake_now
#include "infra/portal_chat.c"
/* Tests may vary identity separately from the rotating session credential. */
#define portal_chat_submit(owner, id, text, result) portal_chat_submit(owner, "owner-a", id, text, result)
static int bus_pushes, bus_result, clear_calls;
int message_bus_push_inbound(const agent_msg_t *message)
{
    assert(!strcmp(message->portal_session, "owner-a"));
    assert(strlen(message->request_id) == 24);
    bus_pushes++;
    if (!bus_result) free(message->content);
    return bus_result;
}
int session_clear(const char *id)
{
    assert(!strcmp(id, "portal-admin")); clear_calls++; return 0;
}
static void reset_process(void)
{
    memset(g_chat, 0, sizeof(g_chat)); g_storage_error = 0; g_loaded = false; g_ordinal = 0;
}
static void id_for(int value, char id[25]) { snprintf(id, 25, "%024x", value); }
static void state_is(const char *id, const char *state)
{
    cJSON *result = portal_chat_get("owner-a", id); assert(result);
    assert(!strcmp(field(result, "state"), state)); cJSON_Delete(result);
}
static void *repeat_submit(void *value)
{
    cJSON *data = NULL;
    assert(!portal_chat_submit("owner-a", value, "concurrent", &data));
    cJSON_Delete(data); return NULL;
}
int main(void)
{
    char id[25]; id_for(1, id);
    cJSON *data = NULL;
    assert(portal_chat_submit(NULL, id, "test", &data) == -EINVAL);
    assert(portal_chat_submit("owner-a", "../bad", "test", &data) == -EINVAL);
    assert(!portal_chat_submit("owner-a", id, "same text", &data)); cJSON_Delete(data);
    assert(bus_pushes == 1);
    assert(!portal_chat_submit("owner-a", id, "same text", &data)); cJSON_Delete(data);
    assert(bus_pushes == 1);
    assert(portal_chat_submit("owner-b", id, "same text", &data) == -EACCES);
    assert(!portal_chat_get("owner-b", id));
    assert(portal_chat_submit("owner-a", id, "changed", &data) == -EEXIST);
    assert(portal_chat_clear() == -EBUSY && clear_calls == 0);
    assert(!portal_chat_begin(id)); assert(portal_chat_can_execute(id));
    assert(portal_chat_begin(id) == -ESTALE);
    now = CHAT_DEADLINE_MS + 1;
    assert(!portal_chat_can_execute(id)); state_is(id, "UNCERTAIN");
    assert(portal_chat_clear() == -EBUSY); /* still executing; expiry cannot clear */
    portal_chat_finish(id, NULL, "TIMEOUT");
    assert(!portal_chat_clear() && clear_calls == 1);
    assert(!portal_chat_submit("owner-a", id, "same text", &data)); cJSON_Delete(data);
    assert(bus_pushes == 1); /* clear never drops dedup state */

    id_for(2, id); now++;
    assert(!portal_chat_submit("owner-a", id, "same text", &data)); cJSON_Delete(data);
    assert(bus_pushes == 2); /* same words, different request */
    reset_process(); state_is(id, "UNCERTAIN"); /* queued requests never auto-replay */
    assert(!portal_chat_submit("owner-a", id, "same text", &data)); cJSON_Delete(data);
    assert(bus_pushes == 2);
    for (int value = 3; value <= 12; value++) {
        id_for(value, id);
        assert(!portal_chat_submit("owner-a", id, "test", &data)); cJSON_Delete(data);
        assert(!portal_chat_begin(id)); portal_chat_finish(id, "reply", NULL);
        state_is(id, "SUCCEEDED");
    }
    int pushes = bus_pushes;
    id_for(1, id);
    assert(portal_chat_submit("owner-a", id, "same text", &data) == -ESTALE);
    assert(bus_pushes == pushes); /* evicted request cannot repeat side effects */
    reset_process(); id_for(12, id); state_is(id, "SUCCEEDED");
    assert(!portal_chat_submit("owner-a", id, "test", &data)); cJSON_Delete(data);
    assert(bus_pushes == pushes);
    bus_result = -1; id_for(13, id);
    assert(!portal_chat_submit("owner-a", id, "queue failure", &data)); cJSON_Delete(data);
    state_is(id, "FAILED"); bus_result = 0;

    id_for(14, id); pushes = bus_pushes;
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, repeat_submit, id));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(bus_pushes == pushes + 1); assert(!portal_chat_begin(id));
    reset_process(); state_is(id, "UNCERTAIN"); /* interrupted RUNNING, not only queued */
    assert(!portal_chat_submit("owner-a", id, "concurrent", &data)); cJSON_Delete(data);
    assert(bus_pushes == pushes + 1);

    /* Fail publication before reservation: do not enqueue or overwrite disk. */
    char temporary[256]; snprintf(temporary, sizeof(temporary), "%s.tmp", PORTAL_CHAT_PATH);
    assert(!mkdir(temporary, 0700));
    id_for(15, id); pushes = bus_pushes;
    assert(portal_chat_submit("owner-a", id, "must not enqueue", &data) < 0);
    assert(bus_pushes == pushes && !find(id)); assert(!rmdir(temporary));
    reset_process(); id_for(14, id); state_is(id, "UNCERTAIN");

    FILE *file = fopen(PORTAL_CHAT_PATH, "r"); assert(file);
    char stored[65536]; size_t length = fread(stored, 1, sizeof(stored) - 1, file); fclose(file);
    stored[length] = 0; assert(!strstr(stored, "owner-a"));
    file = fopen(PORTAL_CHAT_PATH, "w"); assert(file); fputs("corrupt", file); fclose(file);
    reset_process(); id_for(16, id); pushes = bus_pushes;
    assert(portal_chat_submit("owner-a", id, "must not execute", &data) == -EIO);
    assert(!portal_chat_get("owner-a", id)); assert(bus_pushes == pushes);
    printf("PASS portal request ledger: identity, dedup, conflict, deadline, clear race, restart, retirement, queue failure, corrupt storage\n");
    return 0;
}
