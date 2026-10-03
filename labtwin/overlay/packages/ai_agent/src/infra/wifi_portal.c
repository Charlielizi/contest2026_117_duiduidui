/****************************************************************************
 * Gemini-S1 temporary Wi-Fi hotspot provisioning portal
 ****************************************************************************/

#include <nuttx/config.h>

#include "infra/wifi_portal.h"
#include "infra/wifi_radio_guard.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netutils/dhcpd.h>
#include <netutils/netlib.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#ifdef CONFIG_IEEE80211_REALTEK_WIFI
#include <arch/chip/realtek_wlan.h>
#endif

#define PORTAL_IFACE "wlan1"
#define PORTAL_IP "192.168.4.1"
#define PORTAL_NETMASK "255.255.255.0"
#define PORTAL_DNS_PORT 53
#define PORTAL_HTTP_BODY_MAX 384
#define PORTAL_PAGE_MAX 8192

static const char *const g_portal_page_prefix =
    "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Gemini-S1 配网</title><style>body{margin:0;background:#f5f7fa;"
    "font:16px system-ui,sans-serif;color:#1d2733}.card{max-width:420px;"
    "margin:28px auto;padding:24px;background:#fff;border-radius:16px;"
    "box-shadow:0 4px 18px #0002}h1{font-size:24px;margin:0 0 8px}p{line-height:1.55}"
    "label{display:block;margin-top:16px;font-weight:600}input,select{box-sizing:border-box;"
    "width:100%;margin-top:6px;padding:12px;border:1px solid #b9c2cc;border-radius:9px;"
    "font-size:16px}button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:9px;"
    "background:#1769e0;color:#fff;font-size:16px;font-weight:700}.hint{color:#5c6773;font-size:14px}"
    "</style><main class=\"card\"><h1>选择家庭 Wi-Fi</h1>"
    "<p>设备已自动扫描附近网络。请选择 Wi-Fi，再输入密码。</p>"
    "<form method=\"post\" action=\"/provision\"><label>附近的 Wi-Fi"
    "<select name=\"ssid\" required autofocus>";

static const char *const g_portal_page_suffix =
    "</select></label>"
    "<label>Wi-Fi 密码 <span class=\"hint\">（开放网络可留空）</span>"
    "<input name=\"password\" type=\"password\" maxlength=\"63\"></label>"
    "<button type=\"submit\">连接此 Wi-Fi</button></form>"
    "<p class=\"hint\">列表在热点开启前扫描；如目标网络未出现，请关闭后重新开启配网。</p></main>";

static pthread_mutex_t g_portal_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_portal_active;
static bool g_dns_running;
static int g_dns_fd = -1;
static pthread_t g_dns_thread;
static bool g_dns_thread_started;
static bool g_radio_guard_held;
static wifi_portal_network_t g_networks[WIFI_PORTAL_NETWORK_MAX];
static size_t g_network_count;

static bool page_append(char *page, size_t capacity, size_t *used,
    const char *text)
{
    size_t length = strlen(text);

    if (*used + length >= capacity)
        return false;
    memcpy(page + *used, text, length);
    *used += length;
    page[*used] = '\0';
    return true;
}

static bool page_append_escaped(char *page, size_t capacity, size_t *used,
    const char *text)
{
    while (*text) {
        const char *escaped = NULL;
        char one[2] = { *text, '\0' };

        if (*text == '&')
            escaped = "&amp;";
        else if (*text == '<')
            escaped = "&lt;";
        else if (*text == '>')
            escaped = "&gt;";
        else if (*text == '\"')
            escaped = "&quot;";
        else if (*text == '\'')
            escaped = "&#39;";
        if (!page_append(page, capacity, used, escaped ? escaped : one))
            return false;
        text++;
    }
    return true;
}

static char *build_portal_page(void)
{
    wifi_portal_network_t networks[WIFI_PORTAL_NETWORK_MAX];
    char detail[80];
    char *page = malloc(PORTAL_PAGE_MAX);
    size_t count;
    size_t used = 0;
    size_t i;

    if (!page)
        return NULL;
    page[0] = '\0';
    pthread_mutex_lock(&g_portal_lock);
    count = g_network_count;
    memcpy(networks, g_networks, sizeof(networks));
    pthread_mutex_unlock(&g_portal_lock);

    if (!page_append(page, PORTAL_PAGE_MAX, &used, g_portal_page_prefix))
        goto fail;
    if (count == 0) {
        if (!page_append(page, PORTAL_PAGE_MAX, &used,
                "<option value=\"\" disabled selected>未扫描到 Wi-Fi</option>"))
            goto fail;
    }
    for (i = 0; i < count; i++) {
        if (!page_append(page, PORTAL_PAGE_MAX, &used, "<option value=\"") ||
            !page_append_escaped(page, PORTAL_PAGE_MAX, &used,
                networks[i].ssid) ||
            !page_append(page, PORTAL_PAGE_MAX, &used, "\">" ) ||
            !page_append_escaped(page, PORTAL_PAGE_MAX, &used,
                networks[i].ssid))
            goto fail;
        snprintf(detail, sizeof(detail), " · %d dBm%s</option>",
            networks[i].rssi, networks[i].secure ? " · 需密码" : " · 开放");
        if (!page_append(page, PORTAL_PAGE_MAX, &used, detail))
            goto fail;
    }
    if (!page_append(page, PORTAL_PAGE_MAX, &used, g_portal_page_suffix))
        goto fail;
    return page;

fail:
    free(page);
    return NULL;
}

static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t ret = send(fd, data + sent, length - sent, 0);
        if (ret < 0 && errno == EINTR)
            continue;
        if (ret <= 0)
            return -EIO;
        sent += (size_t)ret;
    }
    return 0;
}

static void send_response(int fd, int status, const char *reason,
    const char *body)
{
    char header[192];
    size_t body_len = body ? strlen(body) : 0;
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %lu\r\n\r\n",
        status, reason, (unsigned long)body_len);
    if (header_len > 0 && header_len < (int)sizeof(header))
        send_all(fd, header, (size_t)header_len);
    if (body_len)
        send_all(fd, body, body_len);
}

static bool portal_active(void)
{
    bool active;
    pthread_mutex_lock(&g_portal_lock);
    active = g_portal_active;
    pthread_mutex_unlock(&g_portal_lock);
    return active;
}

static int configure_portal_address(void)
{
    struct in_addr addr;
    struct in_addr netmask;
    struct in_addr lease;
    struct in_addr actual;

    if (inet_pton(AF_INET, PORTAL_IP, &addr) != 1 ||
        inet_pton(AF_INET, PORTAL_NETMASK, &netmask) != 1 ||
        inet_pton(AF_INET, "192.168.4.2", &lease) != 1)
        return -EINVAL;

    /* The Realtek netdev can rewrite its address while transitioning from
     * STA to AP.  Bring it up first, then set and read back the AP address. */
    if (netlib_ifup(PORTAL_IFACE) < 0 ||
        netlib_set_ipv4addr(PORTAL_IFACE, &addr) < 0 ||
        netlib_set_ipv4netmask(PORTAL_IFACE, &netmask) < 0 ||
        netlib_set_dripv4addr(PORTAL_IFACE, &addr) < 0)
        return -errno;
    memset(&actual, 0, sizeof(actual));
    if (netlib_get_ipv4addr(PORTAL_IFACE, &actual) < 0)
        return -errno;
    if (actual.s_addr != addr.s_addr)
        return -EADDRNOTAVAIL;

    /* Keep the lease parameters correct even when a stale global DHCPD
     * configuration survives a previous session.  These setters use host
     * byte order, matching CONFIG_NETUTILS_DHCPD_* values. */
    dhcpd_set_startip(ntohl(lease.s_addr));
    dhcpd_set_routerip(ntohl(addr.s_addr));
    dhcpd_set_netmask(ntohl(netmask.s_addr));
    dhcpd_set_dnsip(ntohl(addr.s_addr));
    return 0;
}

static void clear_portal_address(void)
{
    struct in_addr zero = { .s_addr = INADDR_ANY };
    netlib_set_ipv4addr(PORTAL_IFACE, &zero);
    netlib_set_dripv4addr(PORTAL_IFACE, &zero);
}

static size_t dns_question_end(const uint8_t *packet, size_t length)
{
    size_t offset = 12;
    while (offset < length) {
        uint8_t label_len = packet[offset++];
        if (label_len == 0)
            break;
        if ((label_len & 0xc0) != 0 || offset + label_len > length)
            return 0;
        offset += label_len;
    }
    return offset + 4 <= length ? offset + 4 : 0;
}

static void portal_dns_reply(int fd, const uint8_t *query, size_t query_len,
    const struct sockaddr_in *peer, socklen_t peer_len)
{
    uint8_t reply[512];
    size_t question_end;
    size_t offset;

    if (query_len < 12 || query_len > sizeof(reply) ||
        query[2] & 0x80 || query[4] != 0 || query[5] != 1)
        return;
    question_end = dns_question_end(query, query_len);
    if (!question_end || question_end + 16 > sizeof(reply))
        return;

    memset(reply, 0, sizeof(reply));
    reply[0] = query[0];
    reply[1] = query[1];
    reply[2] = 0x81; /* standard response, recursion desired/available */
    reply[3] = 0x80;
    reply[5] = 1;    /* one question */
    reply[7] = 1;    /* one answer */
    memcpy(reply + 12, query + 12, question_end - 12);
    offset = question_end;
    reply[offset++] = 0xc0; /* answer name points to the question */
    reply[offset++] = 0x0c;
    reply[offset++] = 0x00;
    reply[offset++] = 0x01; /* A */
    reply[offset++] = 0x00;
    reply[offset++] = 0x01; /* IN */
    reply[offset++] = 0x00;
    reply[offset++] = 0x00;
    reply[offset++] = 0x00;
    reply[offset++] = 0x1e; /* short TTL */
    reply[offset++] = 0x00;
    reply[offset++] = 0x04;
    reply[offset++] = 192;
    reply[offset++] = 168;
    reply[offset++] = 4;
    reply[offset++] = 1;
    sendto(fd, reply, offset, 0, (const struct sockaddr *)peer, peer_len);
}

static void *portal_dns_thread(void *arg)
{
    int fd = *(int *)arg;
    uint8_t query[512];

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int poll_ret = poll(&pfd, 1, 250);
        ssize_t received;
        bool running;

        pthread_mutex_lock(&g_portal_lock);
        running = g_dns_running;
        pthread_mutex_unlock(&g_portal_lock);
        if (!running)
            break;
        if (poll_ret <= 0 || !(pfd.revents & POLLIN))
            continue;
        received = recvfrom(fd, query, sizeof(query), 0,
            (struct sockaddr *)&peer, &peer_len);
        if (received > 0)
            portal_dns_reply(fd, query, (size_t)received, &peer, peer_len);
    }
    return NULL;
}

static int portal_dns_start(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTAL_DNS_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    int fd;
    int opt = 1;
    int ret;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -errno;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ret = -errno;
        close(fd);
        return ret;
    }
    pthread_mutex_lock(&g_portal_lock);
    g_dns_fd = fd;
    g_dns_running = true;
    pthread_mutex_unlock(&g_portal_lock);
    ret = pthread_create(&g_dns_thread, NULL, portal_dns_thread, &g_dns_fd);
    if (ret != 0) {
        pthread_mutex_lock(&g_portal_lock);
        g_dns_running = false;
        g_dns_fd = -1;
        pthread_mutex_unlock(&g_portal_lock);
        close(fd);
        return -ret;
    }
    pthread_mutex_lock(&g_portal_lock);
    g_dns_thread_started = true;
    pthread_mutex_unlock(&g_portal_lock);
    return 0;
}

static void portal_dns_stop(void)
{
    int fd;
    pthread_t thread;
    bool join_thread;

    pthread_mutex_lock(&g_portal_lock);
    g_dns_running = false;
    fd = g_dns_fd;
    thread = g_dns_thread;
    join_thread = g_dns_thread_started;
    pthread_mutex_unlock(&g_portal_lock);
    if (join_thread)
        pthread_join(thread, NULL);
    if (fd >= 0)
        close(fd);
    pthread_mutex_lock(&g_portal_lock);
    g_dns_fd = -1;
    g_dns_thread_started = false;
    pthread_mutex_unlock(&g_portal_lock);
}

int wifi_portal_start(const char *ssid, const char *password,
    wifi_portal_scan_cb_t scan, void *arg)
{
    wifi_portal_network_t networks[WIFI_PORTAL_NETWORK_MAX] = { 0 };
    size_t network_count = 0;
    int ret;

    if (!ssid || !password || !ssid[0] || strlen(password) < 8)
        return -EINVAL;
    if (portal_active())
        return -EBUSY;
#ifndef CONFIG_IEEE80211_REALTEK_WIFI
    return -ENOTSUP;
#else
    ret = wifi_radio_guard_acquire();
    if (ret < 0)
        return ret;
    pthread_mutex_lock(&g_portal_lock);
    g_radio_guard_held = true;
    pthread_mutex_unlock(&g_portal_lock);

    if (scan)
        network_count = scan(networks, WIFI_PORTAL_NETWORK_MAX, arg);
    if (network_count > WIFI_PORTAL_NETWORK_MAX)
        network_count = WIFI_PORTAL_NETWORK_MAX;
    pthread_mutex_lock(&g_portal_lock);
    memcpy(g_networks, networks, sizeof(g_networks));
    g_network_count = network_count;
    pthread_mutex_unlock(&g_portal_lock);

    ret = realtek_wl_start_coap(ssid, password, 6);
    if (ret < 0)
        goto fail_guard;
    /* Allow the firmware's AP/netdev transition to settle before assigning
     * the local subnet used by DHCP and the captive portal. */
    usleep(100000);

    ret = configure_portal_address();
    if (ret < 0)
        goto fail_wifi;
    ret = dhcpd_start(PORTAL_IFACE);
    if (ret < 0)
        goto fail_wifi;
    ret = portal_dns_start();
    if (ret < 0) {
        dhcpd_stop();
        goto fail_wifi;
    }
    pthread_mutex_lock(&g_portal_lock);
    g_portal_active = true;
    pthread_mutex_unlock(&g_portal_lock);
    syslog(LOG_INFO, "[wifi_portal] temporary AP started\n");
    return 0;

fail_wifi:
    realtek_wl_stop_coap();
    clear_portal_address();
fail_guard:
    pthread_mutex_lock(&g_portal_lock);
    g_radio_guard_held = false;
    pthread_mutex_unlock(&g_portal_lock);
    wifi_radio_guard_release();
    return ret;
#endif
}

static void portal_stop_internal(bool keep_radio_guard)
{
    bool active;
    bool guard_held;

    pthread_mutex_lock(&g_portal_lock);
    active = g_portal_active || g_dns_thread_started;
    g_portal_active = false;
    guard_held = g_radio_guard_held;
    if (!keep_radio_guard)
        g_radio_guard_held = false;
    pthread_mutex_unlock(&g_portal_lock);
    if (active) {
        portal_dns_stop();
#ifdef CONFIG_NETUTILS_DHCPD
        dhcpd_stop();
#endif
#ifdef CONFIG_IEEE80211_REALTEK_WIFI
        realtek_wl_stop_coap();
#endif
        clear_portal_address();
        syslog(LOG_INFO, "[wifi_portal] temporary AP stopped\n");
    }
    if (guard_held && !keep_radio_guard)
        wifi_radio_guard_release();
}

void wifi_portal_stop(void)
{
    portal_stop_internal(false);
}

bool wifi_portal_is_active(void)
{
    return portal_active();
}

int wifi_portal_begin_sta_handoff(void)
{
    bool guard_held;
    int ret;

    pthread_mutex_lock(&g_portal_lock);
    guard_held = g_radio_guard_held;
    pthread_mutex_unlock(&g_portal_lock);
    if (guard_held)
        return 0;

    ret = wifi_radio_guard_acquire();
    if (ret < 0)
        return ret;
    pthread_mutex_lock(&g_portal_lock);
    g_radio_guard_held = true;
    pthread_mutex_unlock(&g_portal_lock);
    return 0;
}

void wifi_portal_end_sta_handoff(void)
{
    portal_stop_internal(false);
}

static const char *find_header(const char *request, const char *name)
{
    size_t name_len = strlen(name);
    const char *cursor = request;
    while ((cursor = strcasestr(cursor, name)) != NULL) {
        if ((cursor == request || cursor[-1] == '\n') &&
            strncmp(cursor + name_len, ":", 1) == 0)
            return cursor + name_len + 1;
        cursor += name_len;
    }
    return NULL;
}

static int read_content_length(const char *request)
{
    const char *value = find_header(request, "Content-Length");
    int length = 0;
    if (!value)
        return -1;
    while (*value == ' ' || *value == '\t')
        value++;
    while (*value >= '0' && *value <= '9') {
        length = length * 10 + (*value++ - '0');
        if (length > PORTAL_HTTP_BODY_MAX)
            return -1;
    }
    return length;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool form_decode(const char *input, size_t input_len, char *output,
    size_t output_size)
{
    size_t in = 0;
    size_t out = 0;
    if (!output || output_size == 0)
        return false;
    while (in < input_len) {
        char value = input[in++];
        if (value == '+')
            value = ' ';
        else if (value == '%') {
            int hi;
            int lo;
            if (in + 1 >= input_len || (hi = hex_value(input[in++])) < 0 ||
                (lo = hex_value(input[in++])) < 0)
                return false;
            value = (char)((hi << 4) | lo);
        }
        if (value == '\0' || out + 1 >= output_size)
            return false;
        output[out++] = value;
    }
    output[out] = '\0';
    return true;
}

static bool form_value(const char *body, size_t body_len, const char *name,
    char *output, size_t output_size)
{
    size_t name_len = strlen(name);
    const char *cursor = body;
    const char *end = body + body_len;
    while (cursor < end) {
        const char *field_end = memchr(cursor, '&', (size_t)(end - cursor));
        const char *equals;
        if (!field_end)
            field_end = end;
        equals = memchr(cursor, '=', (size_t)(field_end - cursor));
        if (equals && (size_t)(equals - cursor) == name_len &&
            memcmp(cursor, name, name_len) == 0)
            return form_decode(equals + 1, (size_t)(field_end - equals - 1),
                output, output_size);
        cursor = field_end < end ? field_end + 1 : end;
    }
    return false;
}

bool wifi_portal_http_try_handle(int fd, const char *request,
    size_t request_len, wifi_portal_submit_cb_t submit, void *arg)
{
    const char *body;
    size_t header_len;
    int content_len;

    if (!portal_active() || !request || request_len < 5)
        return false;
    if (strncmp(request, "GET ", 4) == 0) {
        char *page = build_portal_page();
        if (page) {
            send_response(fd, 200, "OK", page);
            free(page);
        } else {
            send_response(fd, 500, "Internal Server Error",
                "无法生成 Wi-Fi 列表，请重试");
        }
        return true;
    }
    if (strncmp(request, "POST /provision ", 16) != 0) {
        send_response(fd, 404, "Not Found", "页面不存在");
        return true;
    }
    body = strstr(request, "\r\n\r\n");
    if (!body) {
        send_response(fd, 400, "Bad Request", "请求格式错误");
        return true;
    }
    body += 4;
    header_len = (size_t)(body - request);
    content_len = read_content_length(request);
    if (content_len < 0 || (size_t)content_len > PORTAL_HTTP_BODY_MAX) {
        send_response(fd, 400, "Bad Request", "请求内容无效");
        return true;
    }
    {
        char form[PORTAL_HTTP_BODY_MAX + 1];
        char ssid[33];
        char password[64];
        int ret;
        size_t received = request_len > header_len ? request_len - header_len : 0;

        if (received > (size_t)content_len)
            received = (size_t)content_len;
        if (received)
            memcpy(form, body, received);
        while (received < (size_t)content_len) {
            ssize_t nread = recv(fd, form + received,
                (size_t)content_len - received, 0);
            if (nread <= 0) {
                send_response(fd, 400, "Bad Request", "请求内容不完整，请重试");
                return true;
            }
            received += (size_t)nread;
        }
        form[content_len] = '\0';
        if (!form_value(form, (size_t)content_len, "ssid", ssid, sizeof(ssid)) ||
            !form_value(form, (size_t)content_len, "password", password,
                sizeof(password))) {
            send_response(fd, 400, "Bad Request", "请填写 Wi-Fi 名称");
            return true;
        }
        ret = submit ? submit(ssid, password, arg) : -ENOSYS;
        if (ret == 0)
            send_response(fd, 200, "OK", "<meta charset=\"utf-8\"><h2>已保存，正在连接家庭 Wi-Fi…</h2><p>热点将关闭，请稍候在设备屏幕查看结果。</p>");
        else
            send_response(fd, 400, "Bad Request", "<meta charset=\"utf-8\"><h2>无法保存</h2><p>请检查 Wi-Fi 名称和密码后重试。</p>");
    }
    return true;
}
