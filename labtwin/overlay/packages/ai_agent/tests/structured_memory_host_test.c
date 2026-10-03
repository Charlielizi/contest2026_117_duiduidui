#include "core/structured_memory.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void propose(const char *scope, const char *kind, const char *content,
                    const char *keyword, const char *channel, const char *chat)
{
    char input[1024];
    char output[1024];
    snprintf(input, sizeof(input),
        "{\"scope\":\"%s\",\"kind\":\"%s\",\"content\":\"%s\","
        "\"keywords\":[\"%s\"],\"owner_channel\":\"%s\","
        "\"owner_chat_id\":\"%s\"}",
        scope, kind, content, keyword, channel, chat);
    assert(structured_memory_propose_json(input, output, sizeof(output)) == OK);
    assert(strstr(output, "PENDING_CONFIRMATION") != NULL);
}

static uint64_t owner_hash(const char *channel, const char *chat)
{
    const unsigned char *parts[] = { (const unsigned char *)channel,
        (const unsigned char *)":", (const unsigned char *)chat };
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        for (const unsigned char *p = parts[i]; *p; p++) {
            hash ^= *p;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static void expire_pending(const char *channel, const char *chat)
{
    char path[160];
    char text[1200] = { 0 };
    char *value;
    char *end;
    FILE *file;

    snprintf(path, sizeof(path),
        "/tmp/ai-agent-structured-memory-test/pending/%016" PRIx64 ".json",
        owner_hash(channel, chat));
    file = fopen(path, "r+");
    assert(file != NULL);
    assert(fread(text, 1, sizeof(text) - 1, file) > 0);
    value = strstr(text, "\"expires_epoch\":");
    assert(value != NULL);
    value += strlen("\"expires_epoch\":");
    end = value;
    while (*end && *end != ',' && *end != '}') end++;
    assert(end > value);
    memset(value, ' ', (size_t)(end - value));
    *value = '1';
    assert(fseek(file, 0, SEEK_SET) == 0);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

int main(void)
{
    char output[4096];
    char context[STRUCTURED_MEMORY_CONTEXT_MAX_BYTES + 1];

    assert(structured_memory_init() == OK);

    propose("lab", "research_direction", "实验室长期开展 HPLC 纯度分析。",
            "HPLC", "web", "alice");
    (void)structured_memory_build_context("web", "bob", "HPLC 报告", context,
        sizeof(context));
    assert(strstr(context, "HPLC") == NULL);
    assert(structured_memory_confirm("web", "alice", output, sizeof(output)) == OK);
    assert(strstr(output, "memory confirmed") != NULL);
    {
        struct stat st;
        assert(stat("/tmp/ai-agent-structured-memory-test/lab.json", &st) == 0);
        assert((st.st_mode & 0077) == 0);
    }
    assert(structured_memory_build_context("web", "bob", "HPLC 报告", context,
        sizeof(context)) == OK);
    assert(strstr(context, "HPLC") != NULL);

    propose("user", "reply_style", "回复保持简短中文列表。", "简短",
            "web", "alice");
    assert(structured_memory_confirm("web", "alice", output, sizeof(output)) == OK);
    assert(structured_memory_build_context("web", "alice", "", context,
        sizeof(context)) == OK);
    assert(strstr(context, "简短中文") != NULL);
    assert(structured_memory_build_context("web", "bob", "", context,
        sizeof(context)) == ERROR || strstr(context, "简短中文") == NULL);

    propose("user", "units", "优先使用 mmol 和 mL。", "单位", "web", "alice");
    assert(structured_memory_discard("web", "alice", output, sizeof(output)) == OK);
    assert(structured_memory_confirm("web", "alice", output, sizeof(output)) == ERROR);

    propose("user", "language", "这条草稿必须在过期后失效。", "过期", "web", "expiry");
    expire_pending("web", "expiry");
    assert(structured_memory_confirm("web", "expiry", output, sizeof(output)) == ERROR);

    propose("lab", "method", "旧的同主题方法说明。", "topic-update", "web", "alice");
    assert(structured_memory_confirm("web", "alice", output, sizeof(output)) == OK);
    propose("lab", "method", "新的同主题方法说明。", "topic-update", "web", "alice");
    assert(structured_memory_confirm("web", "alice", output, sizeof(output)) == OK);
    {
        char saved[4096] = { 0 };
        FILE *file = fopen("/tmp/ai-agent-structured-memory-test/lab.json", "r");
        assert(file != NULL);
        assert(fread(saved, 1, sizeof(saved) - 1, file) > 0);
        assert(fclose(file) == 0);
        assert(strstr(saved, "旧的同主题") == NULL);
        assert(strstr(saved, "新的同主题") != NULL);
        file = fopen("/tmp/ai-agent-structured-memory-test/lab.json.tmp", "w");
        assert(file != NULL);
        assert(fputs("{interrupted", file) >= 0);
        assert(fclose(file) == 0);
    }
    assert(structured_memory_build_context("web", "alice", "topic-update", context,
        sizeof(context)) == OK);
    assert(strstr(context, "新的同主题") != NULL);

    for (int i = 0; i < 50; i++) {
        char content[80], keyword[24];
        snprintf(content, sizeof(content), "共享方法偏好条目 %02d", i);
        snprintf(keyword, sizeof(keyword), "kw%02d", i);
        propose("lab", "method", content, keyword, "web", "alice");
        assert(structured_memory_confirm("web", "alice", output, sizeof(output)) == OK);
    }
    assert(structured_memory_handle_command("web", "alice", "/memory", output,
        sizeof(output)) == OK);
    assert(strstr(output, "kw00") == NULL);
    assert(structured_memory_build_context("web", "alice", "kw49", context,
        sizeof(context)) == OK);
    assert(strlen(context) < sizeof(context));

    puts("structured_memory_host_test: PASS");
    return 0;
}
