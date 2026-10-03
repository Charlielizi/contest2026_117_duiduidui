#define _POSIX_C_SOURCE 200809L
#include "infra/web_static.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void fixture(const char *name, const char *body)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", WEB_ROOT, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(body, 1, strlen(body), file) == strlen(body));
    assert(fclose(file) == 0);
}

static void expect_response(const char *request, const char *status,
                            const char *body, int head)
{
    int pair[2];
    char result[4096];
    size_t total = 0;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(web_static_try_handle(pair[0], request, (int)strlen(request)));
    close(pair[0]);
    ssize_t count;
    while ((count = read(pair[1], result + total, sizeof(result) - 1 - total)) > 0)
        total += (size_t)count;
    close(pair[1]);
    result[total] = 0;
    assert(strstr(result, status));
    if (strstr(status, "200")) {
        assert(strstr(result, "Cache-Control: no-cache\r\n"));
        assert(!strstr(result, "immutable"));
        char *payload = strstr(result, "\r\n\r\n");
        assert(payload);
        assert(strcmp(payload + 4, head ? "" : body) == 0);
    }
}

int main(void)
{
    char assets[512];
    snprintf(assets, sizeof(assets), "%s/assets", WEB_ROOT);
    assert(mkdir(assets, 0700) == 0);
    fixture("index.html", "<html>board</html>");
    fixture("assets/recorder-portal.js", "recorder();");
    fixture("assets/index-AbCdEF12.js", "app();");
    fixture("assets/labtwin-logo.png", "image");
    expect_response("GET / HTTP/1.1\r\n\r\n", "200 OK", "<html>board</html>", 0);
    expect_response("GET /assets/recorder-portal.js?v=0123456789abcdef HTTP/1.1\r\n\r\n", "200 OK", "recorder();", 0);
    expect_response("GET /assets/index-AbCdEF12.js HTTP/1.1\r\n\r\n", "200 OK", "app();", 0);
    expect_response("GET /assets/labtwin-logo.png?v=new HTTP/1.1\r\n\r\n", "200 OK", "image", 0);
    expect_response("HEAD /assets/recorder-portal.js HTTP/1.1\r\n\r\n", "200 OK", "", 1);
    expect_response("GET /assets/missing.js HTTP/1.1\r\n\r\n", "404 Not Found", "", 0);
    expect_response("GET /../private HTTP/1.1\r\n\r\n", "400 Bad Request", "", 0);
    char long_path[300];
    memcpy(long_path, "GET /", 5);
    memset(long_path + 5, 'a', 240);
    strcpy(long_path + 245, " HTTP/1.1\r\n\r\n");
    expect_response(long_path, "400 Bad Request", "", 0);
    assert(!web_static_try_handle(-1, "GET /api/v2/status HTTP/1.1", 31));
    puts("WEB_STATIC_HOST_TESTS=PASS (fixed assets, version query, HEAD, missing, traversal, API isolation)");
    return 0;
}
