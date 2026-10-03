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

#include "network_manager.h"
#include "agent_compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netutils/netlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wireless/wapi.h>

static const char* TAG = "netmgr";

static char s_ip_str[INET_ADDRSTRLEN] = "0.0.0.0";

#define NETWORK_PORTAL_AP_IFACE "wlan1"
#define NETWORK_CONFIG_SETTLE_US 250000

bool network_is_connected(void)
{
    struct ifaddrs* ifa_list = NULL;
    if (getifaddrs(&ifa_list) == 0) {
        for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr)
                continue;
            if (ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_name && strcmp(ifa->ifa_name, "lo") == 0)
                continue;
            /* wlan1 is the temporary provisioning AP.  Its 192.168.4.1
             * address is local-only and must not start internet services or
             * terminate the saved-network reconnect worker. */
            if (ifa->ifa_name &&
                strcmp(ifa->ifa_name, NETWORK_PORTAL_AP_IFACE) == 0)
                continue;

            struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
            uint32_t addr = ntohl(sin->sin_addr.s_addr);
            if ((addr >> 24) == 127)
                continue; /* 127.x.x.x loopback */

            /* Skip 0.0.0.0 — interface exists but has no IP yet */
            if (addr == 0)
                continue;

            char address[INET_ADDRSTRLEN];
            if (!inet_ntop(AF_INET, &sin->sin_addr, address,
                           sizeof(address)))
                continue;
            if (strcmp(s_ip_str, address) != 0) {
                syslog(LOG_INFO, "[%s] Found iface %s addr %s\n", TAG,
                    ifa->ifa_name ? ifa->ifa_name : "?", address);
                snprintf(s_ip_str, sizeof(s_ip_str), "%s", address);
            }
            freeifaddrs(ifa_list);
            return true;
        }
        freeifaddrs(ifa_list);
    }

    snprintf(s_ip_str, sizeof(s_ip_str), "%s", "0.0.0.0");
    return false;
}

int network_wait_connected(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return network_is_connected() ? OK : ERROR;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (1) {
        if (network_is_connected()) {
            syslog(LOG_INFO, "[%s] Network connected: %s\n", TAG, s_ip_str);
            return OK;
        }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            syslog(LOG_WARNING, "[%s] Timed out waiting for network\n", TAG);
            return ERROR;
        }

        usleep(500000); /* poll every 500 ms */
    }
}

const char* network_get_ip(void)
{
    network_is_connected(); /* refresh */
    return s_ip_str;
}

/* ── WiFi connect (real hardware only) ───────────────────────── */

#if defined(CONFIG_ARCH_CHIP_GOLDFISH_ARM64) || defined(CONFIG_ARCH_CHIP_QEMU_ARM)
/* On QEMU, virtio-net provides connectivity automatically — but needs
 * ifup/renew */
#include <net/if.h>
#include <nuttx/net/netconfig.h>
#include <sys/ioctl.h>

int network_wifi_connect(const char* iface, const char* ssid,
    const char* pass)
{
    (void)iface;
    (void)ssid;
    (void)pass;
    syslog(LOG_INFO,
        "[%s] QEMU: skipping wifi_connect (virtio-net handles networking)\n",
        TAG);
    return OK;
}

int network_wifi_reconnect(void)
{
    syslog(LOG_INFO, "[%s] QEMU: Initializing eth0...\n", TAG);

    /* Bring up eth0 interface */
    int ret = system("ifup eth0");
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] ifup eth0 failed: %d\n", TAG, ret);
    }

    /* Request DHCP lease */
    ret = system("renew eth0");
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] renew eth0 failed: %d\n", TAG, ret);
    }

    /* Wait a bit for network to come up */
    usleep(500000);

    return network_wait_connected(5000);
}

bool network_wifi_link_is_ready(void)
{
    return network_is_connected();
}

#elif defined(CONFIG_AI_AGENT_NET_RPMSG)
/* ── RPMSG/TUN network via BLE proxy ─────────────────────────── */

#include "config/config_store.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Static state ─────────────────────────────────────────────── */

typedef struct {
    char proxy_mode[16];    /* "usrsock" or "tun" */
    char rpmsg_cpu[16];     /* RPMSG target CPU name */
    int connect_timeout;    /* TLS connect timeout (seconds) */
    int read_timeout;       /* TLS read timeout (seconds) */
    int retry_max;          /* HTTP max retries */
    int retry_base_sec;     /* Retry backoff base (seconds) */
} net_config_t;

static net_config_t g_net_config;
static net_status_t g_net_status;

typedef struct {
    net_state_cb_t cb;
    void* arg;
} net_listener_t;

static net_listener_t g_listeners[NET_MAX_LISTENERS];
static int g_listener_count;

static pthread_mutex_t g_net_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_net_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_poll_thread;
static volatile bool g_poll_running;
static volatile bool g_force_check;

/* ── Internal helpers ─────────────────────────────────────────── */

static void notify_listeners(net_state_t state)
{
    for (int i = 0; i < g_listener_count; i++) {
        if (g_listeners[i].cb) {
            g_listeners[i].cb(state, g_listeners[i].arg);
        }
    }
}

static void set_net_state(net_state_t new_state)
{
    pthread_mutex_lock(&g_net_mutex);
    if (g_net_status.state != new_state) {
        net_state_t old = g_net_status.state;
        g_net_status.state = new_state;

        if (new_state == NET_STATE_CONNECTED) {
            g_net_status.connected_since = time(NULL);
        }

        syslog(LOG_INFO, "[%s] State: %s -> %s\n", TAG,
            old == NET_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED",
            new_state == NET_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED");

        notify_listeners(new_state);
        pthread_cond_broadcast(&g_net_cond);
    }
    pthread_mutex_unlock(&g_net_mutex);
}

/**
 * Check network interfaces via getifaddrs().
 * Returns true if a valid non-loopback IPv4 interface is found.
 * Accepts rpmsg*, tun*, eth* etc. Skips lo* and 127.x.x.x / 0.0.0.0.
 */
static bool check_interfaces(void)
{
    struct ifaddrs* ifa_list = NULL;
    bool found = false;

    if (getifaddrs(&ifa_list) != 0) {
        syslog(LOG_WARNING, "[%s] getifaddrs failed: %d\n", TAG, errno);
        return false;
    }

    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_name && strncmp(ifa->ifa_name, "lo", 2) == 0) {
            continue;
        }

        struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
        uint32_t addr = ntohl(sin->sin_addr.s_addr);

        /* Skip 127.x.x.x loopback */
        if ((addr >> 24) == 127) {
            continue;
        }
        /* Skip 0.0.0.0 — interface exists but has no IP yet */
        if (addr == 0) {
            continue;
        }

        /* Valid interface found */
        pthread_mutex_lock(&g_net_mutex);
        inet_ntop(AF_INET, &sin->sin_addr,
            g_net_status.ip_addr, sizeof(g_net_status.ip_addr));
        if (ifa->ifa_name) {
            strncpy(g_net_status.iface_name, ifa->ifa_name,
                sizeof(g_net_status.iface_name) - 1);
            g_net_status.iface_name[sizeof(g_net_status.iface_name) - 1] = '\0';
        }
        g_net_status.last_check = time(NULL);
        pthread_mutex_unlock(&g_net_mutex);

        /* Also update the shared s_ip_str for network_get_ip() */
        inet_ntop(AF_INET, &sin->sin_addr, s_ip_str, sizeof(s_ip_str));

        syslog(LOG_DEBUG, "[%s] Found iface %s addr %s\n", TAG,
            ifa->ifa_name ? ifa->ifa_name : "?",
            g_net_status.ip_addr);

        found = true;
        break;
    }

    freeifaddrs(ifa_list);
    return found;
}

/* ── Config loading ────────────────────────────────────────────── */

static void load_net_config(void)
{
    char buf[32] = { 0 };

    /* proxy_mode: default "usrsock" */
    if (claw_config_get("net.proxy_mode", g_net_config.proxy_mode,
            sizeof(g_net_config.proxy_mode))
            != OK
        || g_net_config.proxy_mode[0] == '\0') {
        strncpy(g_net_config.proxy_mode, "usrsock",
            sizeof(g_net_config.proxy_mode) - 1);
        g_net_config.proxy_mode[sizeof(g_net_config.proxy_mode) - 1] = '\0';
    }

    /* rpmsg_cpu: default "ap" */
    if (claw_config_get("net.rpmsg_cpu", g_net_config.rpmsg_cpu,
            sizeof(g_net_config.rpmsg_cpu))
            != OK
        || g_net_config.rpmsg_cpu[0] == '\0') {
        strncpy(g_net_config.rpmsg_cpu, "ap",
            sizeof(g_net_config.rpmsg_cpu) - 1);
        g_net_config.rpmsg_cpu[sizeof(g_net_config.rpmsg_cpu) - 1] = '\0';
    }

    /* connect_timeout: default 15 */
    if (claw_config_get("net.connect_timeout", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.connect_timeout = atoi(buf);
    } else {
        g_net_config.connect_timeout = 15;
    }

    /* read_timeout: default 30 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.read_timeout", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.read_timeout = atoi(buf);
    } else {
        g_net_config.read_timeout = 30;
    }

    /* retry_max: default 3 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.retry_max", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.retry_max = atoi(buf);
    } else {
        g_net_config.retry_max = 3;
    }

    /* retry_base_sec: default 2 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.retry_base_sec", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.retry_base_sec = atoi(buf);
    } else {
        g_net_config.retry_base_sec = 2;
    }

    syslog(LOG_INFO,
        "[%s] Config loaded: proxy=%s cpu=%s timeout=%d/%d retry=%d base=%d\n",
        TAG, g_net_config.proxy_mode, g_net_config.rpmsg_cpu,
        g_net_config.connect_timeout, g_net_config.read_timeout,
        g_net_config.retry_max, g_net_config.retry_base_sec);
}

int network_save_proxy_config(const char* mode, const char* cpu_name)
{
    if (!mode || mode[0] == '\0') {
        syslog(LOG_ERR, "[%s] save_proxy_config: mode required\n", TAG);
        return -EINVAL;
    }

    claw_config_set("net.proxy_mode", mode);
    strncpy(g_net_config.proxy_mode, mode,
        sizeof(g_net_config.proxy_mode) - 1);
    g_net_config.proxy_mode[sizeof(g_net_config.proxy_mode) - 1] = '\0';

    if (cpu_name && cpu_name[0] != '\0') {
        claw_config_set("net.rpmsg_cpu", cpu_name);
        strncpy(g_net_config.rpmsg_cpu, cpu_name,
            sizeof(g_net_config.rpmsg_cpu) - 1);
        g_net_config.rpmsg_cpu[sizeof(g_net_config.rpmsg_cpu) - 1] = '\0';
    }

    syslog(LOG_INFO, "[%s] Proxy config saved: mode=%s cpu=%s\n",
        TAG, g_net_config.proxy_mode, g_net_config.rpmsg_cpu);
    return OK;
}

/* ── Config getters ───────────────────────────────────────────── */

int network_get_connect_timeout(void)
{
    return g_net_config.connect_timeout;
}

int network_get_read_timeout(void)
{
    return g_net_config.read_timeout;
}

int network_get_retry_max(void)
{
    return g_net_config.retry_max;
}

int network_get_retry_base_sec(void)
{
    return g_net_config.retry_base_sec;
}

const char* network_get_proxy_mode(void)
{
    return g_net_config.proxy_mode;
}

const char* network_get_rpmsg_cpu(void)
{
    return g_net_config.rpmsg_cpu;
}

/* ── Interface poll thread ────────────────────────────────────── */

static void* iface_poll_thread(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] O74I interface poll thread started\n", TAG);

    /* Startup timeout: 30 seconds to get an IP */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    bool startup_warned = false;

    while (g_poll_running) {
        bool has_ip = check_interfaces();

        if (has_ip) {
            set_net_state(NET_STATE_CONNECTED);
            startup_warned = false;
        } else {
            set_net_state(NET_STATE_DISCONNECTED);

            /* Check startup timeout */
            if (!startup_warned) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
                    + (now.tv_nsec - start.tv_nsec) / 1000000L;
                if (elapsed_ms >= NET_STARTUP_TIMEOUT_MS) {
                    syslog(LOG_WARNING,
                        "[%s] No IP after %d ms startup timeout, "
                        "continuing poll\n",
                        TAG, NET_STARTUP_TIMEOUT_MS);
                    startup_warned = true;
                }
            }
        }

        /* Sleep for poll interval, but wake early on force check */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += NET_POLL_INTERVAL_MS / 1000;
        ts.tv_nsec += (NET_POLL_INTERVAL_MS % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&g_net_mutex);
        while (!g_force_check && g_poll_running) {
            int rc = pthread_cond_timedwait(&g_net_cond, &g_net_mutex, &ts);
            if (rc == ETIMEDOUT) {
                break;
            }
        }
        g_force_check = false;
        pthread_mutex_unlock(&g_net_mutex);
    }

    syslog(LOG_INFO, "[%s] O74I interface poll thread exiting\n", TAG);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

int network_register_listener(net_state_cb_t cb, void* arg)
{
    if (!cb) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_net_mutex);
    if (g_listener_count >= NET_MAX_LISTENERS) {
        pthread_mutex_unlock(&g_net_mutex);
        syslog(LOG_ERR, "[%s] Max listeners (%d) reached\n",
            TAG, NET_MAX_LISTENERS);
        return -ENOMEM;
    }

    g_listeners[g_listener_count].cb = cb;
    g_listeners[g_listener_count].arg = arg;
    g_listener_count++;
    pthread_mutex_unlock(&g_net_mutex);

    return OK;
}

net_state_t network_get_state(void)
{
    net_state_t state;
    pthread_mutex_lock(&g_net_mutex);
    state = g_net_status.state;
    pthread_mutex_unlock(&g_net_mutex);
    return state;
}

int network_get_active_conns(void)
{
    int conns;
    pthread_mutex_lock(&g_net_mutex);
    conns = g_net_status.active_conns;
    pthread_mutex_unlock(&g_net_mutex);
    return conns;
}

int network_get_iob_usage(void)
{
    FILE* fp = NULL;
    int usage_pct = 0;
    char line[128];
    int total = 0;
    int free_cnt = 0;

    fp = fopen("/proc/net/iob", "r");
    if (!fp) {
        syslog(LOG_DEBUG, "[%s] Cannot open /proc/net/iob\n", TAG);
        return 0;
    }

    /* Parse IOB stats — look for total and free counts.
     * Format varies by NuttX version; try common patterns. */
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "  total:%d", &total) == 1) {
            /* found total */
        } else if (sscanf(line, "  free:%d", &free_cnt) == 1) {
            /* found free */
        }
    }

    fclose(fp);

    if (total > 0) {
        usage_pct = ((total - free_cnt) * 100) / total;
        if (usage_pct < 0) {
            usage_pct = 0;
        }
        if (usage_pct > 100) {
            usage_pct = 100;
        }
    }

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.iob_usage_pct = usage_pct;
    pthread_mutex_unlock(&g_net_mutex);

    return usage_pct;
}

int network_reconnect(void)
{
    syslog(LOG_INFO, "[%s] Reconnect requested\n", TAG);

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.state = NET_STATE_DISCONNECTED;
    memset(g_net_status.ip_addr, 0, sizeof(g_net_status.ip_addr));
    memset(g_net_status.iface_name, 0, sizeof(g_net_status.iface_name));
    g_force_check = true;
    pthread_cond_signal(&g_net_cond);
    pthread_mutex_unlock(&g_net_mutex);

    return OK;
}

int network_set_dns(const char* primary, const char* secondary)
{
    FILE* fp = NULL;
    int ret = ERROR;

    if (!primary || primary[0] == '\0') {
        syslog(LOG_ERR, "[%s] set_dns: primary DNS required\n", TAG);
        return -EINVAL;
    }

    fp = fopen("/tmp/resolv.conf", "w");
    if (!fp) {
        syslog(LOG_ERR, "[%s] Cannot open /tmp/resolv.conf: %d\n",
            TAG, errno);
        return -errno;
    }

    fprintf(fp, "nameserver %s\n", primary);
    if (secondary && secondary[0] != '\0') {
        fprintf(fp, "nameserver %s\n", secondary);
    }

    fclose(fp);

    /* Persist to config_store */
    claw_config_set("net.dns_primary", primary);
    if (secondary && secondary[0] != '\0') {
        claw_config_set("net.dns_secondary", secondary);
    }

    syslog(LOG_INFO, "[%s] DNS configured: %s %s\n", TAG,
        primary, (secondary && secondary[0]) ? secondary : "");

    ret = OK;
    return ret;
}

/* ── BLE state callback stub ──────────────────────────────────── */

static void ble_state_callback(bool connected)
{
    syslog(LOG_INFO, "[%s] BLE state: %s\n", TAG,
        connected ? "connected" : "disconnected");

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.ble_connected = connected;
    pthread_mutex_unlock(&g_net_mutex);

    if (!connected) {
        set_net_state(NET_STATE_DISCONNECTED);
    } else {
        /* BLE reconnected — trigger immediate interface check */
        pthread_mutex_lock(&g_net_mutex);
        g_force_check = true;
        pthread_cond_signal(&g_net_cond);
        pthread_mutex_unlock(&g_net_mutex);
    }
}

/* ── RPMSG init ───────────────────────────────────────────────── */

int network_rpmsg_init(void)
{
    int ret = ERROR;
    char dns_primary[64] = { 0 };
    char dns_secondary[64] = { 0 };

    syslog(LOG_INFO, "[%s] O74I: Initializing RPMSG/TUN network\n", TAG);

    /* Load network config from config_store */
    load_net_config();

    /* Initialize state */
    memset(&g_net_status, 0, sizeof(g_net_status));
    g_net_status.state = NET_STATE_DISCONNECTED;
    g_net_status.ble_connected = true; /* assume BLE up at boot */
    g_listener_count = 0;
    g_force_check = false;

    /* Load DNS config from config_store, use defaults if absent */
    if (claw_config_get("net.dns_primary", dns_primary,
            sizeof(dns_primary))
            != OK
        || dns_primary[0] == '\0') {
        strncpy(dns_primary, "223.5.5.5", sizeof(dns_primary) - 1);
        dns_primary[sizeof(dns_primary) - 1] = '\0';
    }

    if (claw_config_get("net.dns_secondary", dns_secondary,
            sizeof(dns_secondary))
            != OK
        || dns_secondary[0] == '\0') {
        strncpy(dns_secondary, "8.8.8.8", sizeof(dns_secondary) - 1);
        dns_secondary[sizeof(dns_secondary) - 1] = '\0';
    }

    /* Configure DNS */
    network_set_dns(dns_primary, dns_secondary);

    /* Start interface poll thread */
    g_poll_running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);

    ret = pthread_create(&g_poll_thread, &attr, iface_poll_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] Failed to create poll thread: %d\n",
            TAG, ret);
        g_poll_running = false;
        return ERROR;
    }

    pthread_setname_np(g_poll_thread, "net_poll");

    /* Register BLE state callback (stub — replace with real BLE API) */
    syslog(LOG_INFO,
        "[%s] BLE callback registered (stub, awaiting BLE framework)\n",
        TAG);
    (void)ble_state_callback; /* suppress unused warning */

    syslog(LOG_INFO, "[%s] O74I network init complete\n", TAG);
    return OK;
}

/* ── Resource guard ───────────────────────────────────────────── */

int network_acquire_resource(uint32_t timeout_ms)
{
    pthread_mutex_lock(&g_net_mutex);

    /* Fast-fail if BLE is disconnected */
    if (!g_net_status.ble_connected) {
        pthread_mutex_unlock(&g_net_mutex);
        return -ENETUNREACH;
    }

    /* Check TCP connection limit and IOB watermark */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (g_net_status.active_conns >= NET_MAX_TCP_CONNS
        || network_get_iob_usage() >= NET_IOB_HIGH_WATERMARK) {
        int rc = pthread_cond_timedwait(&g_net_cond, &g_net_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&g_net_mutex);
            syslog(LOG_WARNING, "[%s] acquire_resource timeout\n", TAG);
            return -ETIMEDOUT;
        }
        /* Re-check BLE after wakeup */
        if (!g_net_status.ble_connected) {
            pthread_mutex_unlock(&g_net_mutex);
            return -ENETUNREACH;
        }
    }

    g_net_status.active_conns++;
    pthread_mutex_unlock(&g_net_mutex);
    return OK;
}

void network_release_resource(void)
{
    pthread_mutex_lock(&g_net_mutex);
    if (g_net_status.active_conns > 0) {
        g_net_status.active_conns--;
    }
    pthread_cond_broadcast(&g_net_cond);
    pthread_mutex_unlock(&g_net_mutex);
}

/* ── WiFi stubs (redirect to rpmsg_init) ──────────────────────── */

int network_wifi_connect(const char* iface, const char* ssid,
    const char* pass)
{
    (void)iface;
    (void)ssid;
    (void)pass;
    syslog(LOG_INFO,
        "[%s] O74I: wifi_connect redirected to rpmsg_init\n", TAG);
    return network_rpmsg_init();
}

int network_wifi_reconnect(void)
{
    syslog(LOG_INFO,
        "[%s] O74I: wifi_reconnect redirected to rpmsg_init\n", TAG);
    return network_rpmsg_init();
}

bool network_wifi_link_is_ready(void)
{
    return network_is_connected();
}

#else
/* Real hardware: use NuttX WAPI ioctl APIs to join WiFi. */
#include "infra/config_store.h"
#include "agent_config.h"
#include <stdio.h>
#include <stdlib.h>

static bool ipv4_addr_is_usable(const struct in_addr* addr)
{
    uint32_t host = ntohl(addr->s_addr);
    return host != INADDR_ANY && (host >> 24) != 127;
}

/* A stale static address from a previous AP must not satisfy set_wifi.
 * Require both a fresh address and a DHCP-provided default gateway for the
 * requested interface. */
static int wait_wifi_lease(const char* dev, uint32_t timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    for (;;) {
        struct in_addr ip = { 0 };
        struct in_addr gateway = { 0 };
        if (netlib_get_ipv4addr(dev, &ip) == 0 &&
            netlib_get_dripv4addr(dev, &gateway) == 0 &&
            ipv4_addr_is_usable(&ip) && ipv4_addr_is_usable(&gateway)) {
            inet_ntop(AF_INET, &ip, s_ip_str, sizeof(s_ip_str));
            return OK;
        }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            return ERROR;
        }

        usleep(500000);
    }
}

/* Use the WAPI ioctl API directly.  On R528, running the wapi command
 * through system() creates a child NSH that can silently skip the PSK/ESSID
 * operations while the agent CLI owns standard input. */
static int configure_wifi_wpa2(const char* dev, const char* ssid,
    const char* pass)
{
    int sock = wapi_make_socket();
    if (sock < 0) {
        return sock;
    }

    int ret = wapi_set_ifup(sock, dev);
    if (ret == OK) {
        /* R528/RTL8723F can be deauthenticated shortly after the WPA2
         * handshake while firmware power save is enabled.  The settings
         * service is interactive, so favor a stable link over idle power.
         */
        int ps_ret = wapi_set_power_save(sock, dev, false);
        if (ps_ret < 0) {
            syslog(LOG_WARNING, "[%s] Unable to disable WiFi power save: %d\n",
                TAG, ps_ret);
        }

        /* The board may have booted with a WAPI profile from usrdata.
         * Drop that association before waiting for the requested AP. */
        wpa_driver_wext_disconnect(sock, dev);
        ret = wapi_set_mode(sock, dev, WAPI_MODE_MANAGED);
    }

    if (ret == OK && pass && pass[0]) {
        ret = wpa_driver_wext_set_auth_param(sock, dev,
            IW_AUTH_WPA_VERSION, IW_AUTH_WPA_VERSION_WPA2);
    }

    if (ret == OK && pass && pass[0]) {
        /* The R528 Realtek WEXT implementation reports -1 for this
         * optional pairwise-cipher ioctl, even though the following CCMP
         * key installation and association succeed.  This is also how the
         * platform wapi command handles it: continue with the explicit
         * CCMP key rather than rejecting a usable WPA2 connection.
         */
        int cipher_ret = wpa_driver_wext_set_auth_param(sock, dev,
            IW_AUTH_CIPHER_PAIRWISE, IW_AUTH_CIPHER_CCMP);
        if (cipher_ret < 0) {
            syslog(LOG_WARNING, "[%s] WAPI pairwise cipher ioctl ignored: %d\n",
                TAG, cipher_ret);
        }
    }

    if (ret == OK && pass && pass[0]) {
        ret = wpa_driver_wext_set_auth_param(sock, dev,
            IW_AUTH_80211_AUTH_ALG, IW_AUTH_ALG_OPEN_SYSTEM);
    }

    if (ret == OK && pass && pass[0]) {
        ret = wpa_driver_wext_set_key_ext(sock, dev, WPA_ALG_CCMP,
            pass, strlen(pass));
    }

    if (ret == OK) {
        int essid_ret = wapi_set_essid(sock, dev, ssid, WAPI_ESSID_ON);
        /* RTL8723F may complete association and still return -1 from the
         * blocking ESSID ioctl when concurrent AP mode is active.  The
         * caller verifies the actual associated BSSID before starting DHCP,
         * so do not turn this advisory return into a false connection error. */
        if (essid_ret < 0) {
            syslog(LOG_WARNING,
                "[%s] WAPI ESSID ioctl returned %d; verifying link state\n",
                TAG, essid_ret);
        }
    }

    close(sock);
    return ret;
}

static int wait_wifi_associated(const char* dev, uint32_t timeout_ms)
{
    int sock = wapi_make_socket();
    if (sock < 0) {
        return sock;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    int ret = ERROR;
    for (;;) {
        struct ether_addr ap = { 0 };
        if (wapi_get_ap(sock, dev, &ap) == OK &&
            memcmp(ap.ether_addr_octet, "\0\0\0\0\0\0",
                   sizeof(ap.ether_addr_octet)) != 0) {
            ret = OK;
            break;
        }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            break;
        }

        usleep(500000);
    }

    close(sock);
    return ret;
}

bool network_wifi_link_is_ready(void)
{
    struct in_addr ip = { 0 };
    struct in_addr gateway = { 0 };
    struct ether_addr ap = { 0 };
    int sock;
    bool associated = false;

    sock = wapi_make_socket();
    if (sock >= 0) {
        associated = wapi_get_ap(sock, "wlan0", &ap) == OK &&
            memcmp(ap.ether_addr_octet, "\0\0\0\0\0\0",
                   sizeof(ap.ether_addr_octet)) != 0;
        close(sock);
    }
    if (!associated)
        return false;

    return netlib_get_ipv4addr("wlan0", &ip) == 0 &&
        netlib_get_dripv4addr("wlan0", &gateway) == 0 &&
        ipv4_addr_is_usable(&ip) && ipv4_addr_is_usable(&gateway);
}

int network_wifi_connect(const char* iface, const char* ssid,
    const char* pass)
{
    if (!ssid || ssid[0] == '\0') {
        syslog(LOG_ERR, "[%s] wifi_connect: SSID required\n", TAG);
        return ERROR;
    }

    const char* dev = (iface && iface[0]) ? iface : "wlan0";
    if (strnlen(ssid, WAPI_ESSID_MAX_SIZE + 1) > WAPI_ESSID_MAX_SIZE ||
        (pass && strnlen(pass, 64) > 63)) {
        syslog(LOG_ERR, "[%s] wifi_connect: invalid SSID or PSK length\n", TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Connecting WiFi SSID=%s on %s\n", TAG, ssid, dev);

    /* Clear any old lease before association.  Complete-image upgrades can
     * otherwise leave an address that makes the old readiness check pass. */
    struct in_addr zero = { .s_addr = INADDR_ANY };
    netlib_set_ipv4addr(dev, &zero);
    netlib_set_dripv4addr(dev, &zero);

    int ret = configure_wifi_wpa2(dev, ssid, pass);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] WAPI association setup failed on %s: %d\n",
            TAG, dev, ret);
        return ERROR;
    }

    if (wait_wifi_associated(dev, 15000) != OK) {
        syslog(LOG_ERR, "[%s] WiFi association timed out on %s\n", TAG, dev);
        return ERROR;
    }

    /* netlib_obtain_ipv4addr returns only after DHCP succeeds or fails. */
    if (netlib_obtain_ipv4addr(dev) < 0) {
        syslog(LOG_ERR, "[%s] DHCP lease failed on %s: %d\n",
            TAG, dev, errno);
        return ERROR;
    }

    int err = wait_wifi_lease(dev, 15000);
    if (err != OK) {
        syslog(LOG_WARNING, "[%s] No valid DHCP lease/route after 15s\n", TAG);
        return err;
    }

    /* Persist only after both DHCP and the default route are valid.  This
     * preserves the previous working credentials if a new join fails.
     * Always write the PSK slot, even for an open AP, to clear stale PSKs.
     * File-backed KVDB commits asynchronously, so consecutive property_set()
     * calls need a settling gap before success implies observable durability. */
    int save_ssid = agent_config_set(AGENT_CFG_KEY_WIFI_SSID, ssid);
    if (save_ssid == OK)
        usleep(NETWORK_CONFIG_SETTLE_US);
    int save_pass = agent_config_set(AGENT_CFG_KEY_WIFI_PASS,
        (pass && pass[0]) ? pass : "");
    if (save_pass == OK)
        usleep(NETWORK_CONFIG_SETTLE_US);
    if (save_ssid != OK || save_pass != OK) {
        syslog(LOG_ERR, "[%s] WiFi connected but credentials were not saved\n",
            TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] WiFi credentials persisted\n", TAG);
    syslog(LOG_INFO, "[%s] WiFi ready: ip=%s (DHCP + gateway)\n", TAG,
        s_ip_str);
    return OK;
}

int network_wifi_reconnect(void)
{
    char ssid[64] = { 0 };
    char pass[128] = { 0 };
    if (agent_config_get(AGENT_CFG_KEY_WIFI_SSID, ssid, sizeof(ssid)) != OK || !ssid[0]) {
        syslog(LOG_WARNING,
            "[%s] No saved WiFi credentials. Use CLI: set_wifi <ssid> <pass>\n",
            TAG);
        return ERROR;
    }
    agent_config_get(AGENT_CFG_KEY_WIFI_PASS, pass, sizeof(pass));
    return network_wifi_connect(NULL, ssid, pass);
}
#endif
