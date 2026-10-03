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

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent_compat.h"
#include "agent_config.h"
#include "device_settings.h"
#include "labtwin/labtwin.h"

#include "core/agent_loop.h"
#include "core/agent_start_gate.h"
#include "core/message_bus.h"
#include "core/message_bus_tap.h"
#include "channels/nsh_commands.h"
#include "infra/config_store.h"
#include "infra/cron_service.h"
#ifdef CONFIG_AI_AGENT_FEISHU
#include "channels/feishu_bot.h"
#endif
#include "channels/ws_server.h"
#include "infra/heartbeat.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "core/memory_store.h"
#include "core/session_mgr.h"
#ifdef CONFIG_AI_AGENT_MQTT
#include "channels/mqtt_channel.h"
#endif
#include "infra/network_manager.h"
#include "infra/wifi_radio_guard.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_client.h"
#include "node/node_manager.h"
#endif
#include "infra/http_proxy.h"
#include "tools/tool_guard.h"
#include "tools/skill_loader.h"
#include "tools/tool_media.h"
#include "tools/tool_registry.h"
#ifdef CONFIG_AI_AGENT_MCP
#include "tools/mcp_bridge.h"
#endif
#include "llm/llm_cache.h"
#include "infra/vela_tls.h"
#if AGENT_SKILL_SYNC_ENABLED
#include "tools/skill_sync.h"
#endif
#include "voice/voice_channel.h"
#include "voice/recording_service.h"
#ifdef CONFIG_AI_AGENT_WAKEWORD
#include "voice/wakeword_session.h"
#include "voice/voice_cloud_guard.h"
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
#include "channels/weixin_channel.h"
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
#include "ui/lvgl_ui_channel.h"
#endif
#ifdef CONFIG_AI_AGENT_BLE_GATT
#include "infra/ble_cmd_handler.h"
#include "infra/ble_gatt.h"
#include <bluetooth.h>
#include <bt_adapter.h>
#endif

static const char* TAG = "agent";

/* ── stdout mutex — shared with nsh_commands.c ──────────────── */
/* Prevents concurrent printf from outbound_dispatch_task and cli_thread
 * which causes adbd shell_service_uv assert (wait_ack != 0). */
pthread_mutex_t g_stdout_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Global shutdown flag ─────────────────────────────────────── */
/* Set by cmd_quit via agent_request_shutdown(); checked by main loop
 * to trigger graceful teardown instead of calling exit(). */
static volatile bool g_shutdown_requested = false;
static pthread_t g_outbound_thread;
static bool g_outbound_thread_started = false;
static pthread_t g_network_watch_thread;
static bool g_network_watch_thread_started = false;
static pthread_mutex_t g_net_services_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_net_services_started = false;

void agent_request_shutdown(void)
{
    g_shutdown_requested = true;
}

bool agent_shutdown_requested(void)
{
    return g_shutdown_requested;
}

/* ── Network watcher (async) ──────────────────────────────────── */

static bool network_services_started(void)
{
    bool started;

    pthread_mutex_lock(&g_net_services_lock);
    started = g_net_services_started;
    pthread_mutex_unlock(&g_net_services_lock);
    return started;
}

static bool wait_interruptible(uint32_t timeout_ms)
{
    while (timeout_ms > 0 && !agent_shutdown_requested()) {
        uint32_t slice_ms = timeout_ms;

        if (slice_ms > AGENT_NETWORK_WATCH_POLL_INTERVAL_MS) {
            slice_ms = AGENT_NETWORK_WATCH_POLL_INTERVAL_MS;
        }
        usleep(slice_ms * 1000U);
        timeout_ms -= slice_ms;
    }

    return !agent_shutdown_requested();
}

static bool wait_for_network_or_shutdown(uint32_t timeout_ms)
{
    while (timeout_ms > 0 && !agent_shutdown_requested()) {
        uint32_t slice_ms = timeout_ms;

        if (network_wifi_link_is_ready()) {
            return true;
        }
        if (slice_ms > AGENT_NETWORK_WATCH_POLL_INTERVAL_MS) {
            slice_ms = AGENT_NETWORK_WATCH_POLL_INTERVAL_MS;
        }
        if (!wait_interruptible(slice_ms)) {
            return false;
        }
        timeout_ms -= slice_ms;
    }

    return network_wifi_link_is_ready();
}

static bool start_network_services_once(void)
{
    bool should_start = false;

    pthread_mutex_lock(&g_net_services_lock);
    if (!g_net_services_started && !agent_shutdown_requested()) {
        g_net_services_started = true;
        should_start = true;
    }
    pthread_mutex_unlock(&g_net_services_lock);

    if (!should_start) {
        return false;
    }

#ifdef CONFIG_AI_AGENT_FEISHU
    if (feishu_bot_start() != OK)
        syslog(LOG_WARNING, "[%s] feishu_bot_start failed\n", TAG);
#endif
    if (agent_loop_start() != OK)
        syslog(LOG_WARNING, "[%s] agent_loop_start failed\n", TAG);
    if (ws_server_start() != OK)
        syslog(LOG_WARNING, "[%s] ws_server_start failed\n", TAG);
#ifdef CONFIG_AI_AGENT_NODE
    if (node_client_start() != OK)
        syslog(LOG_WARNING, "[%s] node_client_start failed\n", TAG);
#endif
#ifdef CONFIG_AI_AGENT_MQTT
    if (mqtt_channel_start() != OK)
        syslog(LOG_WARNING, "[%s] mqtt_channel_start failed\n", TAG);
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
    if (weixin_channel_start() != OK)
        syslog(LOG_WARNING, "[%s] weixin_channel_start failed\n", TAG);
#endif

    syslog(LOG_INFO, "[%s] All network services started\n", TAG);
    return true;
}

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static void net_state_change_cb(net_state_t state, void* arg)
{
    (void)arg;
    if (state == NET_STATE_CONNECTED) {
        syslog(LOG_INFO, "[%s] Network recovered, starting services\n", TAG);
        start_network_services_once();
    } else if (state == NET_STATE_DISCONNECTED) {
        syslog(LOG_WARNING,
            "[%s] Network lost, services may be affected\n", TAG);
    }
}
#endif

/**
 * Runs in a background thread until services are started or shutdown is
 * requested. A late Wi-Fi lease retries the same startup path as cold boot.
 */
static void* network_watch_task(void* arg)
{
    bool radio_busy = false;
    bool bootstrap_waiting = false;
    unsigned int link_failures = 0;
    int reconnect_ret;

    (void)arg;
    syslog(LOG_INFO, "[%s] Network watcher started\n", TAG);

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    if (network_register_listener(net_state_change_cb, NULL) != OK) {
        syslog(LOG_WARNING,
            "[%s] Failed to register network-state listener\n", TAG);
    }
#endif

    while (!agent_shutdown_requested() && !network_services_started()) {
        /* device_settings owns the boot association transaction.  Calling
         * network_wifi_reconnect() before it finishes would see no persisted
         * credentials, reset the radio and can leave the setup AP active. */
        if (device_wifi_bootstrap_in_progress()) {
            if (!bootstrap_waiting) {
                syslog(LOG_INFO,
                    "[%s] Waiting for boot WiFi selection before reconnect\n",
                    TAG);
                bootstrap_waiting = true;
            }
            if (!wait_interruptible(AGENT_NETWORK_WATCH_POLL_INTERVAL_MS))
                break;
            continue;
        }
        if (bootstrap_waiting) {
            syslog(LOG_INFO,
                "[%s] Boot WiFi selection released network watcher\n", TAG);
            bootstrap_waiting = false;
        }
        if (!network_wifi_link_is_ready()) {
            for (unsigned int attempt = 0;
                 attempt < AGENT_NETWORK_WATCH_RECONNECT_ATTEMPTS
                 && !agent_shutdown_requested(); attempt++) {
                if (!wait_interruptible(
                    AGENT_NETWORK_WATCH_RECONNECT_DELAY_MS)) {
                    break;
                }
                if (wifi_radio_guard_try_acquire() < 0) {
                    if (!radio_busy) {
                        syslog(LOG_INFO,
                            "[%s] WiFi provisioning owns radio; reconnect paused\n",
                            TAG);
                        radio_busy = true;
                    }
                    continue;
                }

                if (radio_busy) {
                    syslog(LOG_INFO,
                        "[%s] WiFi radio released; reconnect resumed\n", TAG);
                    radio_busy = false;
                }

                reconnect_ret = network_wifi_reconnect();
                wifi_radio_guard_release();
                if (reconnect_ret == OK || network_wifi_link_is_ready()) {
                    break;
                }

                syslog(LOG_WARNING,
                    "[%s] WiFi attempt %u/%u failed\n", TAG, attempt + 1,
                    AGENT_NETWORK_WATCH_RECONNECT_ATTEMPTS);
            }
        }

        if (agent_shutdown_requested()) {
            break;
        }

        if (wait_for_network_or_shutdown(
            AGENT_NETWORK_WATCH_LINK_TIMEOUT_MS)) {
            syslog(LOG_INFO, "[%s] Network connected: %s\n", TAG,
                network_get_ip());

#if AGENT_SKILL_SYNC_ENABLED
            /* Sync skills before the first agent-loop start; a failed sync
             * keeps local skills and does not prevent normal operation. */
            if (skill_sync_from_bitable() != OK) {
                syslog(LOG_WARNING,
                    "[%s] Bitable skill sync failed, using local skills\n",
                    TAG);
            }
#endif
            start_network_services_once();
            continue;
        }

        if (!agent_shutdown_requested()) {
            syslog(LOG_WARNING,
                "[%s] Network unavailable; retrying service startup later\n",
                TAG);
            wait_interruptible(AGENT_NETWORK_WATCH_RETRY_DELAY_MS);
        }
    }

    /* Network services are one-shot, but the Wi-Fi link is not.  Keep this
     * worker alive after startup so a deauthentication or AP outage cannot
     * leave a stale DHCP address that looks online forever. */
    while (!agent_shutdown_requested() && network_services_started()) {
        if (!wait_interruptible(AGENT_NETWORK_MONITOR_INTERVAL_MS))
            break;

        if (network_wifi_link_is_ready()) {
            link_failures = 0;
            continue;
        }

        if (++link_failures < AGENT_NETWORK_MONITOR_FAILURE_LIMIT)
            continue;

        syslog(LOG_WARNING,
            "[%s] WiFi link lost; starting background reconnect\n", TAG);
        link_failures = 0;
        reconnect_ret = ERROR;

        for (unsigned int attempt = 0;
             attempt < AGENT_NETWORK_WATCH_RECONNECT_ATTEMPTS &&
             !agent_shutdown_requested(); attempt++) {
            if (wifi_radio_guard_try_acquire() < 0) {
                if (!radio_busy) {
                    syslog(LOG_INFO,
                        "[%s] WiFi provisioning owns radio; reconnect paused\n",
                        TAG);
                    radio_busy = true;
                }
            } else {
                if (radio_busy) {
                    syslog(LOG_INFO,
                        "[%s] WiFi radio released; reconnect resumed\n", TAG);
                    radio_busy = false;
                }
                reconnect_ret = network_wifi_reconnect();
                wifi_radio_guard_release();
                if (reconnect_ret == OK && network_wifi_link_is_ready())
                    break;
                syslog(LOG_WARNING,
                    "[%s] WiFi recovery attempt %u/%u failed\n", TAG,
                    attempt + 1, AGENT_NETWORK_WATCH_RECONNECT_ATTEMPTS);
            }

            if (!wait_interruptible(
                    AGENT_NETWORK_WATCH_RECONNECT_DELAY_MS))
                break;
        }

        if (reconnect_ret == OK && network_wifi_link_is_ready()) {
            syslog(LOG_INFO, "[%s] WiFi link recovered: %s\n", TAG,
                network_get_ip());
            if (ws_server_start() != OK) {
                syslog(LOG_WARNING,
                    "[%s] Gateway listener verification failed after reconnect\n",
                    TAG);
            }
        } else if (!agent_shutdown_requested()) {
            syslog(LOG_WARNING,
                "[%s] WiFi remains offline; retrying later\n", TAG);
            wait_interruptible(AGENT_NETWORK_WATCH_RETRY_DELAY_MS);
        }
    }

    syslog(LOG_INFO, "[%s] Network watcher exiting\n", TAG);
    return NULL;
}

#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
/* -- Quickapp mqueue IPC (cross-process bridge) --------------- */

#include <mqueue.h>
#include <fcntl.h>
#include <errno.h>

#define VELACLAW_MQ_QAPP_IN   "/velaclaw_qapp_in"
#define VELACLAW_MQ_QAPP_OUT  "/velaclaw_qapp_out"
#define VELACLAW_MQ_MSG_SIZE  4096
#define VELACLAW_MQ_MAX_MSGS  4

/**
 * Reads requests from the quickapp process via POSIX mqueue and feeds
 * them into the local message_bus inbound queue.
 */
static void *quickapp_mq_listener_task(void *arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] Quickapp mqueue listener started\n", TAG);

    struct mq_attr attr = {
        .mq_maxmsg  = VELACLAW_MQ_MAX_MSGS,
        .mq_msgsize = VELACLAW_MQ_MSG_SIZE,
    };
    mqd_t mq = mq_open(VELACLAW_MQ_QAPP_IN, O_RDONLY | O_CREAT, 0666, &attr);
    if (mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] Failed to open quickapp inbound mqueue: %d\n", TAG, errno);
        return NULL;
    }

    char *buf = (char *)malloc(VELACLAW_MQ_MSG_SIZE);
    if (!buf) {
        syslog(LOG_ERR, "[%s] Failed to allocate mqueue recv buffer\n", TAG);
        mq_close(mq);
        return NULL;
    }

    while (1) {
        ssize_t n = mq_receive(mq, buf, VELACLAW_MQ_MSG_SIZE, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "[%s] mq_receive failed: %d\n", TAG, errno);
            break;
        }
        if (n >= (ssize_t)VELACLAW_MQ_MSG_SIZE) {
            n = VELACLAW_MQ_MSG_SIZE - 1;
        }
        buf[n] = '\0';

        /* Message format: "chat_id\nquery" */
        char *sep = strchr(buf, '\n');
        if (!sep) {
            syslog(LOG_WARNING, "[%s] Malformed quickapp message\n", TAG);
            continue;
        }
        *sep = '\0';
        const char *chat_id = buf;
        const char *query = sep + 1;

        syslog(LOG_INFO, "[%s] Quickapp request: chat_id=%s query=%.40s\n",
               TAG, chat_id, query);

        agent_msg_t msg = {0};
        strncpy(msg.channel, AGENT_CHAN_QUICKAPP, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(query);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    }

    free(buf);
    mq_close(mq);
    return NULL;
}

/**
 * Send a reply to the quickapp process via the outbound mqueue.
 * Called from the outbound dispatch thread in the agent process.
 */
static void quickapp_mq_dispatch(const char *chat_id, const char *content)
{
    struct mq_attr attr = {
        .mq_maxmsg  = VELACLAW_MQ_MAX_MSGS,
        .mq_msgsize = VELACLAW_MQ_MSG_SIZE,
    };
    mqd_t mq = mq_open(VELACLAW_MQ_QAPP_OUT, O_WRONLY | O_CREAT, 0666, &attr);
    if (mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] quickapp dispatch: mq_open failed: %d\n", TAG, errno);
        return;
    }

    char *buf = (char *)malloc(VELACLAW_MQ_MSG_SIZE);
    if (!buf) {
        mq_close(mq);
        return;
    }
    int len = snprintf(buf, VELACLAW_MQ_MSG_SIZE, "%s\n%s", chat_id, content);
    if (len >= (int)VELACLAW_MQ_MSG_SIZE) {
        len = VELACLAW_MQ_MSG_SIZE - 1;
        buf[len] = '\0';
    }

    if (mq_send(mq, buf, len + 1, 0) != 0) {
        syslog(LOG_ERR, "[%s] quickapp dispatch: mq_send failed: %d\n", TAG, errno);
    } else {
        syslog(LOG_INFO, "[%s] quickapp dispatch: reply sent chat_id=%s\n", TAG, chat_id);
    }
    free(buf);
    mq_close(mq);
}
#endif /* CONFIG_FEATURE_SYSTEM_VELACLAW */

/* ── Outbound dispatch ────────────────────────────────────────── */

/* ── Voice failure cooldown — prevent error-message cascade ──── */

static volatile bool s_voice_cooldown;
static time_t s_voice_cooldown_until;

/**
 * Reads messages from the outbound queue and dispatches them to the
 * appropriate channel (FeiShu, WebSocket, CLI, etc.).
 */
static void* outbound_dispatch_task(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] Outbound dispatch started\n", TAG);

    while (!g_shutdown_requested) {
        agent_msg_t msg;
        if (message_bus_pop_outbound(&msg, 1000) != OK)
            continue;
        msg.channel[sizeof(msg.channel) - 1] = '\0';
        msg.chat_id[sizeof(msg.chat_id) - 1] = '\0';

        if (!msg.content) {
            syslog(LOG_WARNING, "[%s] Skip outbound message with NULL content\n", TAG);
            continue;
        }

        syslog(LOG_INFO, "[%s] Dispatching response → %s:%s\n", TAG, msg.channel, msg.chat_id);

        /* Let registered taps intercept before normal dispatch */
        if (mbus_tap_try_deliver(&msg)) {
            free(msg.content);
            continue;
        }

        if (strcmp(msg.channel, AGENT_CHAN_FEISHU) == 0) {
            bool delivered = false;
#ifdef CONFIG_AI_AGENT_FEISHU
            const char* app_id = feishu_get_app_id();
            if (app_id && app_id[0] != '\0') {
                feishu_send_message(msg.chat_id, msg.content);
                delivered = true;
            }
#endif
#ifdef CONFIG_AI_AGENT_NODE
            if (!delivered) {
                delivered = node_client_send_chat_message(
                    msg.channel, msg.chat_id, msg.content) == OK;
            }
#endif
            if (!delivered) {
                syslog(LOG_WARNING,
                    "[%s] Feishu message dropped: no local config "
                    "and no gateway\n",
                    TAG);
            }
        } else if (strcmp(msg.channel, AGENT_CHAN_WEBSOCKET) == 0) {
            ws_server_send_request(msg.chat_id, msg.content, msg.request_id);
#ifdef CONFIG_AI_AGENT_MQTT
        } else if (strcmp(msg.channel, AGENT_CHAN_MQTT) == 0) {
            mqtt_channel_send(msg.chat_id, msg.content);
#endif
        } else if (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
#ifdef CONFIG_AI_AGENT_WAKEWORD
            /* Publish text before checking speech availability. */
            wakeword_session_on_reply(msg.content);
            int tts_gate = voice_cloud_guard_check(CLOUD_TTS);
            if (tts_gate != 0) {
                syslog(LOG_WARNING,
                    "[%s] voice TTS denied: %d\n", TAG, tts_gate);
                if (tts_gate == -ENETDOWN || tts_gate == -EIO)
                    wakeword_session_on_failure(tts_gate == -ENETDOWN ?
                        "network unavailable" : "TTS unavailable");
                free(msg.content);
                continue;
            }
#endif
            /* Check voice cooldown — skip voice dispatch if a recent
             * speak call failed, to prevent error-message cascade. */
            if (s_voice_cooldown && time(NULL) < s_voice_cooldown_until) {
                syslog(LOG_WARNING,
                    "[%s] voice cooldown active, dropping message\n", TAG);
            } else {
                s_voice_cooldown = false;
#ifdef CONFIG_AI_AGENT_WAKEWORD
                wakeword_session_on_tts_begin();
#endif
                int vret = voice_channel_speak(msg.content);
#ifdef CONFIG_AI_AGENT_WAKEWORD
                wakeword_session_on_tts_end(vret);
#endif
                if (vret != 0) {
                    syslog(LOG_ERR, "[%s] voice_channel_speak failed: %d\n", TAG, vret);
                    s_voice_cooldown = true;
                    s_voice_cooldown_until = time(NULL) + 5;
                }
            }
#ifdef CONFIG_AI_AGENT_LVGL_UI
        } else if (strcmp(msg.channel, AGENT_CHAN_LVGL_UI) == 0) {
            int uret = lvgl_ui_channel_send(msg.content);
            if (uret != 0) {
                syslog(LOG_ERR, "[%s] lvgl_ui_channel_send failed: %d\n", TAG, uret);
            }
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
        } else if (strcmp(msg.channel, AGENT_CHAN_WEIXIN) == 0) {
            /* chat_id format: "from_user_id|context_token" */
            char uid[64] = "";
            const char* ctx = "";
            char* sep = strchr(msg.chat_id, '|');
            if (sep) {
                size_t ulen = (size_t)(sep - msg.chat_id);
                if (ulen >= sizeof(uid))
                    ulen = sizeof(uid) - 1;
                memcpy(uid, msg.chat_id, ulen);
                uid[ulen] = '\0';
                ctx = sep + 1;
            } else {
                strncpy(uid, msg.chat_id, sizeof(uid) - 1);
            }
            weixin_channel_send(uid, ctx, msg.content);
#endif
        } else if (strcmp(msg.channel, AGENT_CHAN_SYSTEM) == 0) {
            syslog(LOG_INFO, "[%s] System message [%s]: %.128s\n", TAG, msg.chat_id, msg.content);
#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
        } else if (strcmp(msg.channel, AGENT_CHAN_QUICKAPP) == 0) {
            quickapp_mq_dispatch(msg.chat_id, msg.content);
#endif
        } else if (strcmp(msg.channel, "cli") == 0) {
            pthread_mutex_lock(&g_stdout_lock);
            printf("\n[Agent]: %s\nvela> ", msg.content);
            fflush(stdout);
            pthread_mutex_unlock(&g_stdout_lock);
            syslog(LOG_INFO, "[agent] [Agent]: %s\n", msg.content);
        } else {
            syslog(LOG_WARNING, "[%s] Unknown channel: %s\n", TAG, msg.channel);
        }

        free(msg.content);
    }

    syslog(LOG_INFO, "[%s] Outbound dispatch exiting\n", TAG);
    return NULL;
}

/* ── Entry point ──────────────────────────────────────────────── */

/* ── Startup timing helpers ───────────────────────────────────── */

static inline long boot_ms(struct timespec* t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)((now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L);
}

#define BOOT_LOG(t0, phase, msg) \
    syslog(LOG_INFO, "[%s] [boot +%ldms] " phase ": " msg "\n", TAG, boot_ms(t0))

#define BOOT_LOG_RC(t0, phase, msg, rc) \
    syslog((rc) == OK ? LOG_INFO : LOG_WARNING, \
        "[%s] [boot +%ldms] " phase ": " msg " (rc=%d)\n", TAG, boot_ms(t0), (rc))

static void join_managed_thread(pthread_t thread, bool* started,
    const char* name)
{
    if (!*started) {
        return;
    }

    if (pthread_join(thread, NULL) != 0) {
        syslog(LOG_ERR, "[%s] Failed to join %s thread\n", TAG, name);
        return;
    }

    *started = false;
}

int ai_agent_main(int argc, char* argv[])
{
    int start = agent_start_gate_acquire(argc, argv);
    if (start != 0) return start > 0 ? 0 : start;

    g_shutdown_requested = false;
    g_outbound_thread_started = false;
    g_network_watch_thread_started = false;
    pthread_mutex_lock(&g_net_services_lock);
    g_net_services_started = false;
    pthread_mutex_unlock(&g_net_services_lock);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    syslog(LOG_INFO, "[%s] AI Agent - Vela AI Agent starting (build: %s)\n",
        TAG, AGENT_BUILD_VERSION);

    /* ── Phase 0: Timezone ──────────────────────────────────── */
    /* Set TZ before any time calls so localtime_r returns CST+8.
     * NuttX with CONFIG_LIBC_LOCALTIME+CONFIG_LIBC_ZONEINFO supports
     * both POSIX ("CST-8") and zoneinfo ("Asia/Shanghai") formats. */
    setenv("TZ", AGENT_TIMEZONE, 1);
#ifdef CONFIG_LIBC_LOCALTIME
    tzset();
#endif
    BOOT_LOG(&t0, "P0", "timezone set");

    /* ── Phase 0: Storage Bootstrapping (Auto-mount & Mkdir) ── */

    struct stat st;
    if (stat("/data", &st) != 0) {
        syslog(LOG_INFO, "[%s] Mounting /data as tmpfs for simulation...\n", TAG);
        mount(NULL, "/data", "tmpfs", 0, NULL);
    }

    /* Ensure directory structure exists (No more manual mkdir needed!) */
    mkdir(AGENT_DATA_DIR, 0755);
    mkdir(AGENT_CONFIG_DIR, 0755);
    mkdir(AGENT_MEMORY_DIR, 0755);
    mkdir(AGENT_SESSION_DIR, 0755);
    mkdir(AGENT_SKILLS_DIR, 0755);
    BOOT_LOG(&t0, "P0", "storage ready");

    /* Memory info */
    {
        struct mallinfo mi = mallinfo();
        syslog(LOG_INFO, "[%s] [boot +%ldms] heap: arena=%d free=%d used=%d\n",
            TAG, boot_ms(&t0), mi.arena, mi.fordblks, mi.uordblks);
    }

    /* ── Phase 1: Core infrastructure ──────────────────────── */
    {
        int rc;
        rc = config_store_init();
        BOOT_LOG_RC(&t0, "P1", "config_store_init", rc);

        rc = message_bus_init();
        BOOT_LOG_RC(&t0, "P1", "message_bus_init", rc);
        if (rc != OK)
            return -1;

        rc = memory_store_init();
        BOOT_LOG_RC(&t0, "P1", "memory_store_init", rc);

        rc = session_mgr_init();
        BOOT_LOG_RC(&t0, "P1", "session_mgr_init", rc);

        rc = labtwin_service_init();
        BOOT_LOG_RC(&t0, "P1", "labtwin_service_init", rc);

        rc = recording_service_init();
        BOOT_LOG_RC(&t0, "P1", "recording_service_init", rc);
    }

    /* ── Phase 2: Proxy / networking ───────────────────────── */
    {
        int rc = http_proxy_init();
        BOOT_LOG_RC(&t0, "P2", "http_proxy_init", rc);
    }

    /* ── Phase 3: Application services ─────────────────────── */
    {
        int rc;
#ifdef CONFIG_AI_AGENT_FEISHU
        rc = feishu_bot_init();
        BOOT_LOG_RC(&t0, "P3", "feishu_bot_init", rc);
#endif

        rc = llm_proxy_init();
        BOOT_LOG_RC(&t0, "P3", "llm_proxy_init", rc);

        rc = llm_router_init();
        BOOT_LOG_RC(&t0, "P3", "llm_router_init", rc);

        /* node_manager must init BEFORE tool_registry so it can
         * register itself as a tool provider. */
#ifdef CONFIG_AI_AGENT_NODE
        rc = node_manager_init();
        BOOT_LOG_RC(&t0, "P3", "node_manager_init", rc);
#endif

        rc = tool_registry_init();
        BOOT_LOG_RC(&t0, "P3", "tool_registry_init", rc);

        rc = tool_guard_init();
        BOOT_LOG_RC(&t0, "P3", "tool_guard_init", rc);

        /* Skills must init AFTER tool_registry so executable skills
         * can register themselves as tools. */
        rc = skill_loader_init();
        BOOT_LOG_RC(&t0, "P3", "skill_loader_init", rc);

        rc = agent_loop_init();
        BOOT_LOG_RC(&t0, "P3", "agent_loop_init", rc);

        rc = cron_service_init();
        BOOT_LOG_RC(&t0, "P3", "cron_service_init", rc);

        rc = heartbeat_init();
        BOOT_LOG_RC(&t0, "P3", "heartbeat_init", rc);

#ifdef CONFIG_AI_AGENT_NODE
        rc = node_client_init();
        BOOT_LOG_RC(&t0, "P3", "node_client_init", rc);
#endif

#ifdef CONFIG_AI_AGENT_MQTT
        rc = mqtt_channel_init();
        BOOT_LOG_RC(&t0, "P3", "mqtt_channel_init", rc);
#endif

        rc = voice_channel_init();
        BOOT_LOG_RC(&t0, "P3", "voice_channel_init", rc);

#ifdef CONFIG_AI_AGENT_WAKEWORD
        rc = wakeword_session_init();
        BOOT_LOG_RC(&t0, "P3", "wakeword_session_init", rc);
#endif

#ifdef CONFIG_AI_AGENT_WEIXIN
        rc = weixin_channel_init();
        BOOT_LOG_RC(&t0, "P3", "weixin_channel_init", rc);
#endif

#ifdef CONFIG_AI_AGENT_LVGL_UI
        rc = lvgl_ui_channel_init();
        BOOT_LOG_RC(&t0, "P3", "lvgl_ui_channel_init", rc);
#endif
    }

    /* ── Phase 4: CLI — register commands only, no thread yet ── */
    {
        int rc = nsh_commands_init();
        BOOT_LOG_RC(&t0, "P4", "nsh_commands_init", rc);
    }

    /* ── Phase 5: Network — async, does NOT block ready ────── */

    /* Kick off WiFi reconnect and start outbound dispatch immediately.
     * Network-dependent services (feishu, agent, ws, node) are started
     * inside the network_watch thread once the link comes up. */

    /* Outbound dispatch thread — start before network so queued messages
     * are drained even during the connection window. */
    if (agent_task_create_joinable(outbound_dispatch_task, "outbound",
            AGENT_OUTBOUND_STACK, NULL, AGENT_OUTBOUND_PRIO,
            &g_outbound_thread)
        != OK) {
        syslog(LOG_ERR, "[%s] Failed to start outbound dispatch thread\n", TAG);
        return -1;
    }
    g_outbound_thread_started = true;
    BOOT_LOG(&t0, "P5", "outbound dispatch thread started");

#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
    /* Quickapp mqueue listener - receives requests from quickapp process */
    if (agent_task_create(quickapp_mq_listener_task, "qapp_mq",
                         AGENT_OUTBOUND_STACK, NULL,
                         AGENT_OUTBOUND_PRIO) != OK) {
        syslog(LOG_WARNING, "[%s] Failed to start quickapp mqueue listener\n", TAG);
    }
    BOOT_LOG(&t0, "P5", "quickapp mqueue listener started");
#endif

    /* Cron + heartbeat don't need network */
    cron_service_start();
    heartbeat_start();
    BOOT_LOG(&t0, "P5", "cron + heartbeat started");

#ifdef CONFIG_AI_AGENT_LVGL_UI
    if (lvgl_ui_channel_start() != OK)
        syslog(LOG_WARNING, "[%s] lvgl_ui_channel_start failed\n", TAG);
    BOOT_LOG(&t0, "P5", "lvgl_ui_channel started");
#endif

#ifdef CONFIG_AI_AGENT_BLE_GATT
    /* BLE GATT: ensure adapter enabled, then init with retries */
    {
        extern void ble_cmd_handler_recv(const uint8_t* data, uint16_t len,
            void* user_data);
        bt_instance_t* bt_ins = bluetooth_get_instance();
        if (bt_ins) {
            bt_adapter_state_t state = bt_adapter_get_state(bt_ins);
            if (state < BT_ADAPTER_STATE_BLE_ON) {
                syslog(LOG_INFO, "[%s] BT not ready (state=%d), enabling LE...\n",
                    TAG, (int)state);
                bt_adapter_enable_le(bt_ins);
                sleep(2);
            }
        }

        ble_gatt_config_t ble_cfg = {
            .device_name = "VelaClaw",
            .recv_cb = ble_cmd_handler_recv,
        };
        int rc = -1;
        int attempts = 0;
        while (rc < 0 && attempts < 5) {
            rc = ble_gatt_init(&ble_cfg);
            if (rc < 0) {
                syslog(LOG_WARNING, "[%s] ble_gatt_init attempt %d failed: %d\n",
                    TAG, attempts + 1, rc);
                sleep(3);
            }
            attempts++;
        }
        if (rc == 0) {
            BOOT_LOG(&t0, "P5", "ble_gatt init OK");
        } else {
            BOOT_LOG(&t0, "P5", "ble_gatt FAILED after retries");
        }
    }
#endif

    /* The provisioning hotspot is intentionally available while the STA is
     * offline.  Start the local HTTP/WebSocket listener before the network
     * watcher so http://192.168.4.1 is reachable during first-boot setup.
     * ws_server_start() is idempotent; the online service path below will
     * reuse this listener once the STA obtains a lease. */
    if (ws_server_start() != OK)
        syslog(LOG_WARNING,
               "[%s] local web listener unavailable; will retry online\n", TAG);
    BOOT_LOG(&t0, "P5", "local web listener start attempted");

    /* Network watcher thread: reconnect + wait + start net services */
    if (agent_task_create_joinable(network_watch_task, "net_watch",
            AGENT_OUTBOUND_STACK, NULL, AGENT_OUTBOUND_PRIO,
            &g_network_watch_thread)
        != OK) {
        syslog(LOG_WARNING, "[%s] Failed to start network_watch thread\n", TAG);
    } else {
        g_network_watch_thread_started = true;
    }
    BOOT_LOG(&t0, "P5", "network_watch thread started (async)");

    /* ── Phase 6: CLI thread — all services now in known state ── */
    {
        int rc = nsh_commands_start();
        BOOT_LOG_RC(&t0, "P6", "nsh_commands_start", rc);
    }

    syslog(LOG_INFO, "[%s] [boot +%ldms] AI Agent ready. Type 'help' in NSH for commands.\n",
        TAG, boot_ms(&t0));

    /* Block main thread until shutdown is requested */
    while (!g_shutdown_requested) {
        sleep(1);
    }

    syslog(LOG_INFO, "[%s] Shutdown requested — stopping services...\n", TAG);

    /* Wake all threads blocked on message bus before stopping services */
    message_bus_wakeup();

    /* Do not let a late network lease start services after teardown begins. */
    join_managed_thread(g_network_watch_thread,
        &g_network_watch_thread_started, "network_watch");

    /* ── Graceful teardown (reverse of startup order) ──────── */

    /* Phase 5 services (network-dependent) */
#ifdef CONFIG_AI_AGENT_WEIXIN
    weixin_channel_stop();
#endif
#ifdef CONFIG_AI_AGENT_NODE
    node_client_stop();
#endif
#ifdef CONFIG_AI_AGENT_MQTT
    mqtt_channel_stop();
#endif
    ws_server_stop();

#ifdef CONFIG_AI_AGENT_LVGL_UI
    lvgl_ui_channel_stop();
#endif

    /* Phase 5 services (non-network) */
#ifdef CONFIG_AI_AGENT_WAKEWORD
    wakeword_session_shutdown();
#endif
    cron_service_stop();
    heartbeat_stop();

    /* Agent loop owns tool buffers and can return from a bounded LLM or
     * tool call after shutdown is requested. Wait before shared cleanup. */
    agent_loop_shutdown();
    join_managed_thread(g_outbound_thread, &g_outbound_thread_started,
        "outbound");

    /* Cleanup — reverse order of init */
    tool_media_cleanup();
#ifdef CONFIG_AI_AGENT_MCP
    mcp_bridge_cleanup();
#endif
    tool_guard_cleanup();
    tool_registry_cleanup();
    llm_cache_cleanup();
    vela_tls_pool_cleanup();
    message_bus_destroy();

    syslog(LOG_INFO, "[%s] Shutdown complete.\n", TAG);

    agent_start_gate_release();
    return 0;
}
