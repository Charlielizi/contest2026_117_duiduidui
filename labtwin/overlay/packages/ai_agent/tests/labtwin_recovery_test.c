#include "labtwin/labtwin.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAPSHOT "/tmp/labtwin-m2-test-20260714-v2/experiments/M2A/snapshot.json"

static void make_snapshot_stale(void)
{
    FILE *file = fopen(SNAPSHOT, "r");
    char buffer[16384];
    size_t size;
    cJSON *root;
    char *text;
    assert(file != NULL);
    size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[size] = 0;
    root = cJSON_Parse(buffer);
    assert(root != NULL);
    cJSON_ReplaceItemInObject(root, "state", cJSON_CreateString("READY"));
    cJSON_ReplaceItemInObject(root, "last_event_seq", cJSON_CreateNumber(0));
    text = cJSON_PrintUnformatted(root);
    assert(text != NULL);
    file = fopen(SNAPSHOT, "w");
    assert(file != NULL);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    fclose(file);
    free(text);
    cJSON_Delete(root);
}

int main(void)
{
    char out[16384];
    make_snapshot_stale();
    assert(labtwin_service_init() == 0);
    assert(labtwin_experiment_get_json(
        "{\"experiment_id\":\"M2A\"}", out, sizeof(out)) == 0);
    assert(strstr(out, "\"state\":\"COMPLETED\"") != NULL);
    assert(strstr(out, "color darkened") != NULL);
    assert(strstr(out, "RECOVERY_ERROR") == NULL);
    printf("labtwin_recovery_test: PASS\n");
    return 0;
}
