#include "infra/web_static.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef WEB_ROOT
#define WEB_ROOT "/resource/labtwin-admin"
#endif
#define WEB_PATH_MAX 256
#define WEB_SEND_BUFFER_SIZE 4096

static int send_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(fd, bytes + offset, length - offset, 0);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        offset += (size_t)sent;
    }
    return 0;
}

static const char *mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    return "application/octet-stream";
}

static void send_error(int fd, int code, const char *status)
{
    char body[96];
    char header[256];
    int body_len = snprintf(body, sizeof(body),
                            "{\"error\":\"%s\"}", status);
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", code, status, body_len);
    send_all(fd, header, (size_t)header_len);
    send_all(fd, body, (size_t)body_len);
}

static bool safe_request_path(const char *input, char *output, size_t size,
                              bool *asset)
{
    size_t length;
    const char *query;
    if (!input || input[0] != '/' || strstr(input, "..") ||
        strchr(input, '\\') || strchr(input, '%'))
        return false;
    query = strchr(input, '?');
    length = query ? (size_t)(query - input) : strlen(input);
    if (length == 0 || length >= size)
        return false;
    memcpy(output, input, length);
    output[length] = '\0';
    *asset = strncmp(output, "/assets/", 8) == 0;
    return true;
}

bool web_static_try_handle(int fd, const char *request, int request_len)
{
    char method[8] = { 0 };
    char request_path[WEB_PATH_MAX] = { 0 };
    char clean_path[WEB_PATH_MAX] = { 0 };
    char file_path[WEB_PATH_MAX] = { 0 };
    char header[512];
    unsigned char *buffer;
    struct stat st;
    FILE *file;
    bool head;
    bool asset;
    int header_len;

    (void)request_len;
    if (!request || sscanf(request, "%7s %255s", method, request_path) != 2)
        return false;
    head = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head)
        return false;
    if (strncmp(request_path, "/api/", 5) == 0 ||
        strncmp(request_path, "/labtwin", 8) == 0 ||
        strncmp(request_path, "/admin-events", 13) == 0)
        return false;
    if (!safe_request_path(request_path, clean_path, sizeof(clean_path), &asset)) {
        send_error(fd, 400, "Bad Request");
        return true;
    }

    if (strcmp(clean_path, "/") == 0)
        snprintf(file_path, sizeof(file_path), "%s/index.html", WEB_ROOT);
    else {
        size_t root_len = strlen(WEB_ROOT);
        size_t path_len = strlen(clean_path);
        if (root_len + path_len >= sizeof(file_path)) {
            send_error(fd, 400, "Bad Request");
            return true;
        }
        memcpy(file_path, WEB_ROOT, root_len);
        memcpy(file_path + root_len, clean_path, path_len + 1);
    }

    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (asset) {
            send_error(fd, 404, "Not Found");
            return true;
        }
        snprintf(file_path, sizeof(file_path), "%s/index.html", WEB_ROOT);
        if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            send_error(fd, 503, "Web UI Unavailable");
            return true;
        }
    }

    file = fopen(file_path, "rb");
    if (!file) {
        send_error(fd, 500, "Read Error");
        return true;
    }
    buffer = malloc(WEB_SEND_BUFFER_SIZE);
    if (!buffer) {
        fclose(file);
        send_error(fd, 503, "Out of Memory");
        return true;
    }
    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; connect-src 'self' ws: wss:; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'\r\n"
        "Connection: close\r\n\r\n",
        mime_type(file_path), (long)st.st_size,
        /* Fixed-name extensions and logos can change with every firmware.
         * Revalidate all resources; content-versioned URLs also bypass stale
         * year-long entries left by older firmware in existing browsers. */
        "no-cache");
    if (send_all(fd, header, (size_t)header_len) == 0 && !head) {
        size_t count;
        while ((count = fread(buffer, 1, WEB_SEND_BUFFER_SIZE, file)) > 0) {
            if (send_all(fd, buffer, count) != 0)
                break;
        }
    }
    fclose(file);
    free(buffer);
    return true;
}
