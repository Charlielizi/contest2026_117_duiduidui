#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "tools/tool_files.c"
int main(void)
{
    char output[256], input[1024];
    const char *ledger = AGENT_DATA_DIR "/portal-chat.json";
    assert(!mkdir(AGENT_DATA_DIR "/portal-chat.json.retired", 0700));
    assert(!symlink(AGENT_DATA_DIR "/portal-chat.json.retired", AGENT_DATA_DIR "/alias"));
    assert(is_structured_memory_path(ledger));
    assert(is_structured_memory_path(AGENT_DATA_DIR "//portal-chat.json.tmp"));
    assert(is_structured_memory_path(AGENT_DATA_DIR "/alias/000000000000000000000001"));
    assert(!is_structured_memory_path(AGENT_DATA_DIR "/notes.txt"));
    snprintf(input, sizeof(input), "{\"path\":\"%s\",\"content\":\"tamper\"}", ledger);
    assert(tool_write_file_execute(input, output, sizeof(output)) == ERROR);
    snprintf(input, sizeof(input), "{\"path\":\"%s\"}", ledger);
    assert(tool_read_file_execute(input, output, sizeof(output)) == ERROR);
    snprintf(input, sizeof(input), "{\"path\":\"%s/alias/000000000000000000000001\",\"content\":\"tamper\"}", AGENT_DATA_DIR);
    assert(tool_write_file_execute(input, output, sizeof(output)) == ERROR);
    puts("PASS portal request state is private to trusted APIs, including directory aliases");
    return 0;
}
