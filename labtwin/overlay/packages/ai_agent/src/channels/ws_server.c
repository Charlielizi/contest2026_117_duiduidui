/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "channels/ws_server.h"
#include "core/message_bus.h"
#include "labtwin/labtwin_dashboard.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_manager.h"
#endif
#include "agent_compat.h"
#include "agent_config.h"
#ifdef CONFIG_AI_AGENT_REST_API
#include "infra/api_handler.h"
#include "infra/admin_api.h"
#include "infra/admin_auth.h"
#include "infra/portal_chat.h"
#endif
#include "infra/web_static.h"
#include "infra/mdns_responder.h"
#include "infra/wifi_portal.h"
#include "device_settings.h"

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* POSIX sockets */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/types.h>

/* mbedTLS SHA-1 + Base64 */
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

static const char* TAG = "ws";

/* ── WS Magic GUID ────────────────────────────────────────────── */

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define LABTWIN_AGENT_PATH "/agent"
#define LABTWIN_AGENT_PROTOCOL "labtwin.agent.v1"

/* ── Client table ─────────────────────────────────────────────── */

typedef struct {
    int fd;
    char chat_id[32];
    bool active;
    bool dashboard;
    bool portal_agent;
    char portal_owner[65];
} ws_client_t;

static ws_client_t s_clients[AGENT_WS_MAX_CLIENTS];
static pthread_mutex_t s_clients_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_handshake_clients;

#define AGENT_HANDSHAKE_MAX 8
#define AGENT_HANDSHAKE_TIMEOUT_SECONDS 5
#define AGENT_WS_FRAME_BUFFER_SIZE 4096
#define AGENT_ACCEPT_STACK_SIZE (6 * 1024)
#define AGENT_WS_CLIENT_PRIORITY 80

static int s_listen_fd = -1;
static int s_http_listen_fd = -1;
static volatile bool s_running = false;

/* ── Client helpers (call with mtx held) ──────────────────────── */

static ws_client_t* find_by_fd_locked(int fd)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd)
            return &s_clients[i];
    }
    return NULL;
}

static ws_client_t* find_by_chat_id_locked(const char* chat_id)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && !s_clients[i].dashboard &&
            strcmp(s_clients[i].chat_id, chat_id) == 0)
            return &s_clients[i];
    }
    return NULL;
}

static ws_client_t* add_client_locked(int fd, bool dashboard, bool portal_agent)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            s_clients[i].active = true;
            s_clients[i].dashboard = dashboard;
            s_clients[i].portal_agent = portal_agent;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            syslog(LOG_INFO, "[%s] Client connected: %s (fd=%d)\n", TAG,
                s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    return NULL;
}

static void remove_client_locked(int fd)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            syslog(LOG_INFO, "[%s] Client disconnected: %s\n", TAG,
                s_clients[i].chat_id);
            s_clients[i].active = false;
            s_clients[i].dashboard = false;
            s_clients[i].portal_agent = false;
            s_clients[i].fd = -1; /* invalidate fd before closing */
            close(fd);
            return;
        }
    }
}

/* ── WS handshake ─────────────────────────────────────────────── */

/**
 * Compute Sec-WebSocket-Accept = base64(SHA1(key + GUID))
 */
static int make_accept_key(const char* key, char* out, size_t out_size)
{
    char combined[256];
    int clen = snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);
    if (clen <= 0 || clen >= (int)sizeof(combined))
        return -1;

    unsigned char sha[20];
    if (mbedtls_sha1((const unsigned char*)combined, (size_t)clen, sha) != 0)
        return -1;

    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char*)out, out_size, &written, sha,
            sizeof(sha))
        != 0)
        return -1;
    out[written] = '\0';
    return (int)written;
}

static int socket_send_all(int fd, const void* data, size_t length)
{
    const unsigned char* bytes = data;
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

static int ws_portal_submit(const char *ssid, const char *password, void *arg)
{
    (void)arg;
    return device_wifi_connect(ssid, password);
}

/**
 * WebSocket handshake using pre-read buffer.
 * Read full HTTP upgrade request, extract Sec-WebSocket-Key.
 * Returns 0 on success; sends 101 response.
 */
static int do_ws_handshake_ex(int fd, const char* buf, int buf_len,
    char* chat_id_out, size_t chat_id_size, bool dashboard, bool portal_agent)
{

    /* Extract Sec-WebSocket-Key */
    char* key_hdr = strcasestr(buf, "\r\nSec-WebSocket-Key: ");
    if (!key_hdr) {
        syslog(LOG_WARNING, "[%s] No Sec-WebSocket-Key\n", TAG);
        return -1;
    }
    key_hdr += 21;
    char* eol = strstr(key_hdr, "\r\n");
    if (!eol)
        return -1;

    char ws_key[128] = { 0 };
    size_t klen = (size_t)(eol - key_hdr);
    if (klen >= sizeof(ws_key))
        return -1;
    memcpy(ws_key, key_hdr, klen);
    ws_key[klen] = '\0';

    /* Compute accept */
    char accept[64] = { 0 };
    if (make_accept_key(ws_key, accept, sizeof(accept)) < 0)
        return -1;

    /* Send 101 */
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "%s"
        "\r\n",
        accept, dashboard ?
            "Sec-WebSocket-Protocol: " LABTWIN_DASHBOARD_PROTOCOL "\r\n" :
            (portal_agent ? "Sec-WebSocket-Protocol: " LABTWIN_AGENT_PROTOCOL "\r\n" : ""));
    if (socket_send_all(fd, resp, (size_t)rlen) != 0)
        return -1;

    /* Default chat_id from fd (may be overridden by first message) */
    snprintf(chat_id_out, chat_id_size, "ws_%d", fd);
    return 0;
}

/* ── WS frame encode/decode ───────────────────────────────────── */

/**
 * Send a text frame (server → client, no masking).
 * The caller must serialize writes for this fd.
 */
static int ws_send_frame(int fd, const char* payload, size_t len)
{
    unsigned char hdr[10];
    int hdr_len = 0;

    hdr[0] = 0x81; /* FIN + text opcode */
    if (len < 126) {
        hdr[1] = (unsigned char)len;
        hdr_len = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)(len & 0xFF);
        hdr_len = 4;
    } else {
        syslog(LOG_ERR, "[%s] Payload too large: %d\n", TAG, (int)len);
        return -1;
    }

    if (socket_send_all(fd, hdr, (size_t)hdr_len) != 0)
        return -1;
    if (socket_send_all(fd, payload, len) != 0)
        return -1;
    return 0;
}

/**
 * Read and decode one WS frame; place text payload into buf (NUL-terminated).
 * Returns payload length on success, 0 on close frame, -1 on error.
 */
static int ws_recv_frame(int fd, char* buf, size_t buf_size)
{
    unsigned char b0, b1;
    if (recv(fd, &b0, 1, MSG_WAITALL) != 1)
        return -1;
    if (recv(fd, &b1, 1, MSG_WAITALL) != 1)
        return -1;

    int opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    size_t plen = b1 & 0x7F;

    if (opcode == 0x8)
        return 0; /* Close frame */

    if (opcode == 0x9 || opcode == 0xA) {
        /* Ping (0x9) or Pong (0xA) — must consume payload before returning,
         * otherwise leftover bytes corrupt the next frame read. */
        if (plen == 126) {
            unsigned char ext[2];
            if (recv(fd, ext, 2, MSG_WAITALL) != 2)
                return -1;
            plen = ((size_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            return -1;
        }

        unsigned char pp_mask[4] = { 0 };
        if (masked) {
            if (recv(fd, pp_mask, 4, MSG_WAITALL) != 4)
                return -1;
        }

        if (plen > 125) {
            syslog(LOG_ERR,
                "[%s] Invalid control frame payload length: %d\n",
                TAG, (int)plen);
            return -1;
        }

        /* Read full control payload so ping can be echoed in pong. */
        unsigned char ctrl_payload[125] = { 0 };
        if (plen > 0) {
            if (recv(fd, ctrl_payload, plen, MSG_WAITALL) != (int)plen)
                return -1;

            if (masked) {
                for (size_t i = 0; i < plen; i++) {
                    ctrl_payload[i] ^= pp_mask[i % 4];
                }
            }
        }

        /* Reply with pong if it was a ping */
        if (opcode == 0x9) {
            unsigned char pong_hdr[2] = { 0x8A, (unsigned char)plen };
            send(fd, pong_hdr, 2, 0);
            if (plen > 0) {
                send(fd, ctrl_payload, plen, 0);
            }
        }
        return -2;
    }

    if (opcode != 0x1 && opcode != 0x2) {
        /* Unknown opcode — skip */
        return -2;
    }

    if (plen == 126) {
        unsigned char ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2)
            return -1;
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        syslog(LOG_ERR, "[%s] 64-bit frame length not supported\n", TAG);
        return -1;
    }

    unsigned char mask_key[4] = { 0 };
    if (masked) {
        if (recv(fd, mask_key, 4, MSG_WAITALL) != 4)
            return -1;
    }

    if (plen >= buf_size) {
        syslog(LOG_ERR, "[%s] Frame too large: %d\n", TAG, (int)plen);
        return -1;
    }

    int received = 0;
    while ((size_t)received < plen) {
        int n = recv(fd, buf + received, plen - received, 0);
        if (n <= 0)
            return -1;
        received += n;
    }

    if (masked) {
        for (size_t i = 0; i < plen; i++) {
            ((unsigned char*)buf)[i] ^= mask_key[i % 4];
        }
    }

    buf[plen] = '\0';
    return (int)plen;
}

/* ── Per-client thread ────────────────────────────────────────── */

typedef struct {
    int fd;
} client_arg_t;

typedef struct {
    int fd;
} listener_arg_t;

void ws_server_pending_operation(const char *owner, const char *id)
{
    char frame[128];
    snprintf(frame, sizeof(frame), "{\"type\":\"pending_operation\",\"operation_id\":\"%s\"}", id);
    pthread_mutex_lock(&s_clients_mtx);
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++)
        if (s_clients[i].active && s_clients[i].portal_agent && !strcmp(s_clients[i].portal_owner, owner))
            ws_send_frame(s_clients[i].fd, frame, strlen(frame));
    pthread_mutex_unlock(&s_clients_mtx);
}

static void request_slot_release(void)
{
    pthread_mutex_lock(&s_clients_mtx);
    if (s_handshake_clients > 0)
        s_handshake_clients--;
    pthread_mutex_unlock(&s_clients_mtx);
}

static void* client_thread(void* arg)
{
    client_arg_t ca = *(client_arg_t*)arg;
    free(arg);
    int fd = ca.fd;

    /* Peek HTTP headers to decide: REST API or WebSocket */
    char* peek_buf = malloc(2048);
    if (!peek_buf) {
        request_slot_release();
        close(fd);
        return NULL;
    }
    int peek_total = 0;
    while (peek_total < 2048 - 1) {
        int n = recv(fd, peek_buf + peek_total,
            2048 - 1 - peek_total, 0);
        if (n <= 0) {
            request_slot_release();
            free(peek_buf);
            close(fd);
            return NULL;
        }
        peek_total += n;
        peek_buf[peek_total] = '\0';
        if (strstr(peek_buf, "\r\n\r\n"))
            break;
    }

    if (wifi_portal_http_try_handle(fd, peek_buf, (size_t)peek_total,
            ws_portal_submit, NULL)) {
        request_slot_release();
        free(peek_buf);
        close(fd);
        return NULL;
    }

    /* Try REST API (config/skills/logs) */
#ifdef CONFIG_AI_AGENT_REST_API
    if (api_try_handle(fd, peek_buf, peek_total)) {
        request_slot_release();
        free(peek_buf);
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(fd);
        return NULL;
    }
#endif

    /* Classify real-time endpoints before the SPA's unknown-path fallback.
     * Otherwise GET /agent is served index.html with HTTP 200, so browser
     * WebSocket clients never reach the Upgrade handshake. */
    bool dashboard = strncmp(peek_buf, "GET " LABTWIN_DASHBOARD_PATH " ",
                             strlen("GET " LABTWIN_DASHBOARD_PATH " ")) == 0 ||
        strncmp(peek_buf, "GET " LABTWIN_DASHBOARD_PATH "?",
                strlen("GET " LABTWIN_DASHBOARD_PATH "?")) == 0;
    bool portal_agent = strncmp(peek_buf, "GET " LABTWIN_AGENT_PATH " ",
                                strlen("GET " LABTWIN_AGENT_PATH " ")) == 0 ||
        strncmp(peek_buf, "GET " LABTWIN_AGENT_PATH "?",
                strlen("GET " LABTWIN_AGENT_PATH "?")) == 0;
    if (portal_agent)
        syslog(LOG_INFO, "[%s] Agent WebSocket upgrade request\n", TAG);

    if (!dashboard && !portal_agent &&
        web_static_try_handle(fd, peek_buf, peek_total)) {
        request_slot_release();
        free(peek_buf);
        close(fd);
        return NULL;
    }

    /* /labtwin remains read-only; /agent is an administrator-only chat path. */
    bool dashboard_admin_session = false;
    char portal_owner[65] = "";
#ifdef CONFIG_AI_AGENT_REST_API
    if (dashboard) {
        admin_session_view_t admin_session;
        dashboard_admin_session = admin_auth_session(peek_buf, &admin_session);
    }
    if (portal_agent) {
        admin_session_view_t admin_session;
        if (!admin_auth_session(peek_buf, &admin_session) ||
            !admin_api_origin_valid(peek_buf)) {
            static const char denied[] =
                "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            socket_send_all(fd, denied, sizeof(denied) - 1);
            request_slot_release();
            free(peek_buf);
            close(fd);
            return NULL;
        }
        snprintf(portal_owner, sizeof(portal_owner), "%s", admin_session.token);
    }
#endif

    /* Not REST API - proceed with WebSocket handshake using pre-read buffer */
    char chat_id[32];
    if (do_ws_handshake_ex(fd, peek_buf, peek_total,
            chat_id, sizeof(chat_id), dashboard, portal_agent)
        != 0) {
        syslog(LOG_WARNING, "[%s] Handshake failed for fd=%d\n", TAG, fd);
        request_slot_release();
        free(peek_buf);
        close(fd);
        return NULL;
    }
    /* HTTP responses remain inside the bounded request slot.  A successful
     * WebSocket has its own AGENT_WS_MAX_CLIENTS table, so release the
     * transient slot before entering the long-lived frame loop. */
    request_slot_release();
    free(peek_buf);
    char* frame_buf = malloc(AGENT_WS_FRAME_BUFFER_SIZE);
    if (!frame_buf) {
        close(fd);
        return NULL;
    }
    /* Handshake timeout protects incomplete HTTP clients.  Established
     * WebSockets are long-lived and must return to blocking reads. */
    {
        struct timeval no_timeout = { 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &no_timeout, sizeof(no_timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &no_timeout, sizeof(no_timeout));
    }

    /* Register client */
    pthread_mutex_lock(&s_clients_mtx);
    ws_client_t* client = add_client_locked(fd, dashboard, portal_agent);
    if (!client) {
        pthread_mutex_unlock(&s_clients_mtx);
        syslog(LOG_WARNING, "[%s] Max clients reached, closing fd=%d\n", TAG, fd);
        /* Politely close */
        unsigned char close_frame[2] = { 0x88, 0x00 };
        send(fd, close_frame, 2, 0);
        free(frame_buf);
        close(fd);
        return NULL;
    }
    snprintf(client->chat_id, sizeof(client->chat_id), "%s", chat_id);
    if (portal_agent)
        snprintf(client->chat_id, sizeof(client->chat_id), "%s", "portal-admin");
    if (portal_agent)
        snprintf(client->portal_owner, sizeof(client->portal_owner), "%s", portal_owner);
    pthread_mutex_unlock(&s_clients_mtx);

    /* Send connect.challenge so Nodes can identify themselves */
#ifdef CONFIG_AI_AGENT_NODE
    if (!dashboard && !portal_agent)
        node_manager_send_challenge(fd);
#endif

    /* Read loop — the frame buffer is heap-backed to preserve client stack. */
    labtwin_dashboard_session_t dashboard_session;
    labtwin_dashboard_session_init(&dashboard_session, dashboard);
    if (dashboard && dashboard_admin_session)
        dashboard_session.authenticated = true;
    while (s_running) {
        int n = ws_recv_frame(fd, frame_buf, AGENT_WS_FRAME_BUFFER_SIZE);
        if (n < 0 && n != -2)
            break; /* -2 = ignored opcode, not an error */
        if (n == 0)
            break; /* clean close */
        if (n < 0)
            continue; /* ignored frame */

        /* Dashboard messages never enter the Node or chat protocols. */
        char* dashboard_response = NULL;
        if (labtwin_dashboard_handle(&dashboard_session, frame_buf, (size_t)n,
                                     &dashboard_response)) {
            if (dashboard_response) {
                int send_ret = ws_send_frame(fd, dashboard_response,
                                             strlen(dashboard_response));
                free(dashboard_response);
                if (send_ret != 0)
                    break;
            }
            continue;
        }

        /* Try Node protocol first — if handled, skip chat processing */
#ifdef CONFIG_AI_AGENT_NODE
        if (!portal_agent && node_manager_handle_message(fd, frame_buf, n))
            continue;
#endif

        /* Parse JSON message */
        cJSON* root = cJSON_Parse(frame_buf);
        if (!root) {
            syslog(LOG_WARNING, "[%s] Invalid JSON from fd=%d (%d bytes): %.200s\n",
                TAG, fd, n, frame_buf);
            continue;
        }

        cJSON* type_item = cJSON_GetObjectItem(root, "type");
        cJSON* content_item = cJSON_GetObjectItem(root, "content");

        if (cJSON_IsString(type_item) && strcmp(type_item->valuestring, "message") == 0 && cJSON_IsString(content_item)) {

            /* Determine/update chat_id */
            cJSON* cid_item = cJSON_GetObjectItem(root, "chat_id");
            if (!portal_agent && cJSON_IsString(cid_item) &&
                !strcmp(cid_item->valuestring, "portal-admin")) {
                cJSON_Delete(root);
                break;
            }
            pthread_mutex_lock(&s_clients_mtx);
            ws_client_t* c = find_by_fd_locked(fd);
            if (c) {
                if (!portal_agent && cJSON_IsString(cid_item)) {
                    strncpy(c->chat_id, cid_item->valuestring, sizeof(c->chat_id) - 1);
                }
                strncpy(chat_id, c->chat_id, sizeof(chat_id) - 1);
            }
            pthread_mutex_unlock(&s_clients_mtx);

            if (!portal_agent)
                syslog(LOG_INFO, "[%s] WS msg from %s: %.40s\n", TAG, chat_id,
                    content_item->valuestring);

            agent_msg_t msg = { 0 };
#ifdef CONFIG_AI_AGENT_REST_API
            if (portal_agent && !admin_auth_token_valid(portal_owner)) {
                cJSON_Delete(root);
                break;
            }
#endif
            if (portal_agent) {
#ifdef CONFIG_AI_AGENT_REST_API
                const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "request_id"));
                cJSON *state = NULL;
                int accepted = portal_chat_submit("administrator", portal_owner, id, content_item->valuestring, &state);
                cJSON *response = cJSON_CreateObject();
                cJSON_AddStringToObject(response, "type", accepted ? "error" : "status");
                if (id) cJSON_AddStringToObject(response, "request_id", id);
                if (state) cJSON_AddItemToObject(response, "request", state);
                if (accepted) cJSON_AddStringToObject(response, "code", "CHAT_REQUEST_REJECTED");
                char *json = cJSON_PrintUnformatted(response); cJSON_Delete(response);
                if (json) { ws_send_frame(fd, json, strlen(json)); free(json); }
#endif
                cJSON_Delete(root);
                continue;
            }
            strncpy(msg.channel, AGENT_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
            msg.content = strdup(content_item->valuestring);
            if (msg.content && message_bus_push_inbound(&msg) != OK)
                free(msg.content);
        }
        cJSON_Delete(root);
    }

    /* Notify node manager before removing client */
#ifdef CONFIG_AI_AGENT_NODE
    if (!dashboard && !portal_agent)
        node_manager_on_disconnect(fd);
#endif

    /* Remove and close */
    pthread_mutex_lock(&s_clients_mtx);
    remove_client_locked(fd);
    pthread_mutex_unlock(&s_clients_mtx);

    return NULL;
}

/* ── Accept loop ──────────────────────────────────────────────── */

static void* accept_thread(void* arg)
{
    listener_arg_t listener = *(listener_arg_t *)arg;
    int listen_fd = listener.fd;
    free(arg);
    while (s_running) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int cfd;
        struct timeval io_timeout = {
            .tv_sec = AGENT_HANDSHAKE_TIMEOUT_SECONDS,
            .tv_usec = 0,
        };

        if (!s_running || listen_fd < 0)
            break;

        cfd = accept(listen_fd, (struct sockaddr*)&addr, &addr_len);
        if (cfd < 0) {
            int accept_errno = errno;
            if (!s_running)
                break;
            /* A Wi-Fi interface teardown can invalidate the listening
             * socket underneath this detached worker.  Leave the old loop
             * instead of spinning forever (or accepting on a recycled fd)
             * while ws_server_start() rebuilds the listener after DHCP. */
            if (accept_errno == EBADF || accept_errno == ENOTSOCK ||
                accept_errno == EINVAL)
                break;
            if (accept_errno == EINTR)
                continue;
            if (accept_errno != EAGAIN && accept_errno != EWOULDBLOCK) {
                syslog(LOG_WARNING, "[%s] accept failed, errno=%d\n",
                       TAG, accept_errno);
            }
            usleep(50000);
            continue;
        }

        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO,
                   &io_timeout, sizeof(io_timeout));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO,
                   &io_timeout, sizeof(io_timeout));
        pthread_mutex_lock(&s_clients_mtx);
        if (s_handshake_clients >= AGENT_HANDSHAKE_MAX) {
            pthread_mutex_unlock(&s_clients_mtx);
            close(cfd);
            continue;
        }
        s_handshake_clients++;
        pthread_mutex_unlock(&s_clients_mtx);

        client_arg_t* ca = malloc(sizeof(client_arg_t));
        if (!ca) {
            pthread_mutex_lock(&s_clients_mtx);
            s_handshake_clients--;
            pthread_mutex_unlock(&s_clients_mtx);
            close(cfd);
            continue;
        }
        ca->fd = cfd;

        pthread_t tid;
        pthread_attr_t attr;
        struct sched_param priority = {
            .sched_priority = AGENT_WS_CLIENT_PRIORITY,
        };
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, AGENT_WS_CLIENT_STACK);
        pthread_attr_setschedparam(&attr, &priority);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        if (pthread_create(&tid, &attr, client_thread, ca) != 0) {
            pthread_mutex_lock(&s_clients_mtx);
            s_handshake_clients--;
            pthread_mutex_unlock(&s_clients_mtx);
            free(ca);
            close(cfd);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

static int start_listener(int port, int *fd_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    int flags;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (fd < 0)
        return ERROR;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(fd, AGENT_WS_MAX_CLIENTS) < 0) {
        syslog(LOG_ERR, "[%s] listener failed on port %d, errno=%d\n",
               TAG, port, errno);
        close(fd);
        return ERROR;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    *fd_out = fd;
    return OK;
}

static int start_accept_thread(int fd)
{
    pthread_t tid;
    pthread_attr_t attr;
    listener_arg_t *arg = malloc(sizeof(*arg));
    int rc;
    if (!arg)
        return ERROR;
    arg->fd = fd;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, AGENT_ACCEPT_STACK_SIZE);
    rc = pthread_create(&tid, &attr, accept_thread, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(arg);
        return ERROR;
    }
    return OK;
}

static int start_listener_thread(int port, int *fd_out)
{
    int fd;

    if (start_listener(port, &fd) != OK)
        return ERROR;
    if (start_accept_thread(fd) != OK) {
        syslog(LOG_ERR, "[%s] accept thread failed on port %d\n", TAG, port);
        close(fd);
        return ERROR;
    }
    *fd_out = fd;
    return OK;
}

static bool listener_is_accepting(int fd)
{
    int accepting = 0;
    socklen_t len = sizeof(accepting);

    return fd >= 0 &&
        getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &len) == 0 &&
        accepting != 0;
}

static int ensure_listener_thread(int port, int *fd_slot)
{
    int attempt;

    if (listener_is_accepting(*fd_slot))
        return OK;

    if (*fd_slot >= 0) {
        close(*fd_slot);
        *fd_slot = -1;
    }

    /* A detached accept worker may need one polling interval to observe the
     * invalid descriptor and release the underlying listener PCB. */
    for (attempt = 0; attempt < 10; attempt++) {
        if (start_listener_thread(port, fd_slot) == OK)
            return OK;
        usleep(100000);
    }
    return ERROR;
}

static void* mdns_start_thread(void* arg)
{
    int attempt;
    (void)arg;
    /* Wi-Fi association and DHCP complete after ai_agent on some boots.
     * Keep retrying outside the gateway startup path instead of permanently
     * losing the .local service after one early EAGAIN. */
    for (attempt = 0; s_running && attempt < 120; attempt++) {
        if (labtwin_mdns_start() == 0) {
            syslog(LOG_INFO, "[%s] mDNS responder ready after %d attempt(s)\n",
                   TAG, attempt + 1);
            return NULL;
        }
        sleep(1);
    }
    if (s_running)
        syslog(LOG_WARNING, "[%s] mDNS responder unavailable after retries\n",
               TAG);
    return NULL;
}

static int start_mdns_thread(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    int rc;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4096);
    rc = pthread_create(&thread, &attr, mdns_start_thread, NULL);
    pthread_attr_destroy(&attr);
    return rc == 0 ? OK : ERROR;
}

/* ── Public API ───────────────────────────────────────────────── */

int ws_server_start(void)
{
    /* The local listener starts before internet connectivity so the
     * provisioning portal works on an offline boot.  The network-service
     * path calls this function again after DHCP.  Removing the temporary AP
     * can discard INADDR_ANY listener PCBs without updating these cached fd
     * integers, so validate SO_ACCEPTCONN and rebuild only dead listeners. */
    if (s_running) {
        if (AGENT_WS_PORT != 80) {
            if (ensure_listener_thread(80, &s_http_listen_fd) != OK) {
                syslog(LOG_WARNING,
                       "[%s] port 80 unavailable after network refresh\n", TAG);
            }
        }
        if (ensure_listener_thread(AGENT_WS_PORT, &s_listen_fd) != OK) {
            syslog(LOG_WARNING,
                   "[%s] port %d unavailable after network refresh\n",
                   TAG, AGENT_WS_PORT);
        }
        if (s_listen_fd >= 0 || s_http_listen_fd >= 0)
            syslog(LOG_INFO, "[%s] Gateway listeners verified after DHCP\n",
                   TAG);
        return s_listen_fd >= 0 || s_http_listen_fd >= 0 ? OK : ERROR;
    }

    memset(s_clients, 0, sizeof(s_clients));
#ifdef CONFIG_AI_AGENT_REST_API
    admin_api_init();
#endif
    s_running = true;
    s_handshake_clients = 0;
    /* Port 80 is the provisioning and management entry point.  Start it
     * first and keep each listener independent so a legacy-port failure
     * cannot leave a bound socket without an accept thread. */
    if (AGENT_WS_PORT != 80 &&
        start_listener_thread(80, &s_http_listen_fd) != OK) {
        syslog(LOG_WARNING, "[%s] port 80 unavailable\n", TAG);
    }
    if (start_listener_thread(AGENT_WS_PORT, &s_listen_fd) != OK) {
        syslog(LOG_WARNING, "[%s] port %d unavailable\n", TAG,
               AGENT_WS_PORT);
    }
    if (s_listen_fd < 0 && s_http_listen_fd < 0) {
        syslog(LOG_ERR, "[%s] no gateway listener could be started\n", TAG);
        s_running = false;
        return ERROR;
    }

    if (s_http_listen_fd >= 0 || AGENT_WS_PORT == 80)
        syslog(LOG_INFO, "[%s] Gateway started on ports 80 and %d\n", TAG,
               AGENT_WS_PORT);
    else
        syslog(LOG_WARNING, "[%s] Gateway started on port %d without port 80\n",
               TAG, AGENT_WS_PORT);
    if (start_mdns_thread() != OK)
        syslog(LOG_WARNING, "[%s] mDNS retry worker unavailable\n", TAG);
    return OK;
}

int ws_server_send(const char* chat_id, const char* text)
{
    return ws_server_send_request(chat_id, text, NULL);
}

int ws_server_send_request(const char* chat_id, const char* text, const char *request_id)
{
    if ((s_listen_fd < 0 && s_http_listen_fd < 0) || !s_running)
        return ERROR;

    /* Build JSON response */
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);
    if (request_id && request_id[0]) cJSON_AddStringToObject(resp, "request_id", request_id);
    char* json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str)
        return ERROR;

    pthread_mutex_lock(&s_clients_mtx);
    ws_client_t* client = find_by_chat_id_locked(chat_id);
    int rc = -1;
    if (client) {
        rc = ws_send_frame(client->fd, json_str, strlen(json_str));
        if (rc != 0) {
            syslog(
                LOG_WARNING,
                "[%s] Send failed to %s (fd will be cleaned up by client thread)\n",
                TAG, chat_id);
            /* Don't remove/close here — the client_thread owns the fd lifecycle.
             * The recv() in client_thread will fail and trigger cleanup. */
        }
    } else {
        syslog(LOG_WARNING, "[%s] No WS client for chat_id=%s\n", TAG, chat_id);
    }
    pthread_mutex_unlock(&s_clients_mtx);

    free(json_str);
    return (client == NULL) ? ERROR : (rc == 0 ? OK : ERROR);
}

int ws_server_stop(void)
{
    s_running = false;
    labtwin_mdns_stop();
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    if (s_http_listen_fd >= 0) {
        close(s_http_listen_fd);
        s_http_listen_fd = -1;
    }
    syslog(LOG_INFO, "[%s] WebSocket server stopped\n", TAG);
    return OK;
}

int ws_server_client_count(void)
{
    int count = 0;
    pthread_mutex_lock(&s_clients_mtx);
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++)
        if (s_clients[i].active)
            count++;
    pthread_mutex_unlock(&s_clients_mtx);
    return count;
}
