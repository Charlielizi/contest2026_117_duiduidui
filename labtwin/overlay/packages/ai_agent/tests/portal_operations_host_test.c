#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/random.h>
#include "infra/portal_operations.h"
#include "tools/tool_guard.h"
#include "labtwin/labtwin.h"
static bool private_mode = true;
static int deleted;
void portal_test_random_buf(void *buffer, size_t size)
{
    assert(getrandom(buffer, size, 0) == (ssize_t)size);
}
#define arc4random_buf portal_test_random_buf
bool admin_auth_token_valid(const char *token) { return token && (!strcmp(token, "admin-a") || !strcmp(token, "admin-b")); }
bool admin_auth_private_open(void) { return private_mode; }
bool admin_auth_verify_password(const char *password) { return password && !strcmp(password, "test-only-password"); }
int admin_delete_experiment(const char *id, uint64_t seq, const char *source) { (void)id; (void)seq; assert(!strcmp(source, "portal-agent")); deleted++; return 0; }
tool_guard_result_t tool_guard_check(const char *name, const char *input, size_t length)
{ (void)name; (void)input; (void)length; return GUARD_ALLOW; }
void tool_guard_record_call(const char *name) { (void)name; }
int tool_registry_execute(const char *name, const char *input, char *out, size_t size)
{ (void)name; (void)input; snprintf(out, size, "read-only test"); return 0; }
/* Include only the operation service to exercise its reboot/expiry boundaries. */
#include "../src/infra/portal_operations.c"

static cJSON *proposal(const char *tool, const char *input)
{
    char out[1024];
    assert(portal_tool_execute(tool, input, "admin-a", out, sizeof(out)) == 0);
    assert(strstr(out, "CONFIRMATION_REQUIRED") && !strstr(out, "\"token\""));
    cJSON *response = cJSON_Parse(out);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(response, "operation_id"));
    cJSON *op = portal_operations_get("admin-a", id);
    assert(op && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(op, "state")), "PENDING"));
    assert(!portal_operations_get("admin-b", id));
    cJSON_Delete(response); return op;
}
static void resolve(cJSON *op, bool cancel, const char *expected, bool password)
{
    cJSON *body = cJSON_CreateObject(), *result = NULL;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(op, "id"));
    cJSON_AddStringToObject(body, "token", cJSON_GetStringValue(cJSON_GetObjectItem(op, "token")));
    cJSON_AddStringToObject(body, "confirm_experiment_id", "PA");
    if (password) cJSON_AddStringToObject(body, "password", "test-only-password");
    assert(portal_operation_resolve("admin-b", id, body, cancel, &result) == -EACCES);
    assert(portal_operation_resolve("admin-a", id, body, cancel, &result) == 0);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(result, "state")), expected));
    cJSON_Delete(result); result = NULL;
    assert(portal_operation_resolve("admin-a", id, body, cancel, &result) == 0);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(result, "state")), expected));
    cJSON_Delete(result); cJSON_Delete(body); cJSON_Delete(op);
}
int main(void)
{
    char out[16384];
    mkdir(LABTWIN_ROOT "/experiments", 0700);
    assert(labtwin_experiment_create_json("{\"experiment_id\":\"PA\",\"name\":\"test\",\"steps\":[\"one\"]}", out, sizeof(out), "test") == 0);
    cJSON *op = proposal("experiment_transition", "{\"experiment_id\":\"PA\",\"action\":\"cancel\"}");
    labtwin_experiment_get_json("{\"experiment_id\":\"PA\"}", out, sizeof(out));
    assert(strstr(out, "READY"));
    resolve(op, true, "CANCELLED", false);
    op = proposal("experiment_transition", "{\"experiment_id\":\"PA\",\"action\":\"cancel\"}");
    assert(labtwin_experiment_transition_json("{\"experiment_id\":\"PA\",\"action\":\"start\"}", out, sizeof(out), "test") == 0);
    resolve(op, false, "FAILED", false); /* stale sequence */
    op = proposal("experiment_transition", "{\"experiment_id\":\"PA\",\"action\":\"complete\"}");
    resolve(op, false, "APPLIED", false);
    op = proposal("experiment_delete", "{\"experiment_id\":\"PA\"}");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(op, "id"));
    pending_for(id)->deadline = 0;
    resolve(op, false, "EXPIRED", false);
    private_mode = false;
    op = proposal("experiment_delete", "{\"experiment_id\":\"PA\"}");
    cJSON *bad = cJSON_CreateObject(), *result = NULL;
    id = cJSON_GetStringValue(cJSON_GetObjectItem(op, "id"));
    cJSON_AddStringToObject(bad, "token", cJSON_GetStringValue(cJSON_GetObjectItem(op, "token")));
    cJSON_AddStringToObject(bad, "confirm_experiment_id", "PA");
    assert(portal_operation_resolve("admin-a", id, bad, false, &result) == -EACCES);
    cJSON_Delete(bad);
    resolve(op, false, "APPLIED", true); assert(deleted == 1);
    private_mode = true;
    op = proposal("experiment_delete", "{\"experiment_id\":\"PA\"}");
    resolve(op, false, "APPLIED", false); assert(deleted == 2);
    op = proposal("experiment_delete", "{\"experiment_id\":\"PA\"}");
    id = cJSON_GetStringValue(cJSON_GetObjectItem(op, "id"));
    for (int i = 0; i < LEDGER_MAX; i++) if (!strcmp(g_ledger[i].id, id)) g_ledger[i].state = OP_EXECUTING;
    assert(persist() == 0);
    g_loaded = false; memset(g_pending, 0, sizeof(g_pending));
    resolve(op, false, "UNCERTAIN", false); assert(deleted == 2);
    puts("portal_operations_host_test: PASS");
    return 0;
}
