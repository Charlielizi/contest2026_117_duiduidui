#include "infra/mdns_responder.h"

#include "infra/config_store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <netutils/netlib.h>

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define MDNS_PACKET_MAX 512

static int g_mdns_fd = -1;
static volatile bool g_mdns_running;

static void put_u16(unsigned char *packet, size_t *offset, uint16_t value)
{
    packet[(*offset)++] = (unsigned char)(value >> 8);
    packet[(*offset)++] = (unsigned char)value;
}

static void put_u32(unsigned char *packet, size_t *offset, uint32_t value)
{
    packet[(*offset)++] = (unsigned char)(value >> 24);
    packet[(*offset)++] = (unsigned char)(value >> 16);
    packet[(*offset)++] = (unsigned char)(value >> 8);
    packet[(*offset)++] = (unsigned char)value;
}

static bool put_name(unsigned char *packet, size_t *offset, size_t capacity,
                     const char *name)
{
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t length = dot ? (size_t)(dot - label) : strlen(label);
        if (length == 0 || length > 63 || *offset + length + 1 >= capacity)
            return false;
        packet[(*offset)++] = (unsigned char)length;
        memcpy(packet + *offset, label, length);
        *offset += length;
        if (!dot) break;
        label = dot + 1;
    }
    packet[(*offset)++] = 0;
    return true;
}

static bool packet_has_name(const unsigned char *packet, size_t size,
                            const char *name)
{
    unsigned char encoded[128];
    size_t length = 0;
    size_t i;
    if (!put_name(encoded, &length, sizeof(encoded), name) || length > size)
        return false;
    for (i = 0; i + length <= size; i++)
        if (memcmp(packet + i, encoded, length) == 0)
            return true;
    return false;
}

static size_t build_host_response(unsigned char *packet, size_t capacity,
                                  const char *hostname, struct in_addr address)
{
    size_t offset = 0;
    memset(packet, 0, capacity);
    put_u16(packet, &offset, 0);       /* transaction id */
    put_u16(packet, &offset, 0x8400);  /* authoritative response */
    put_u16(packet, &offset, 0);       /* questions */
    put_u16(packet, &offset, 1);       /* answers */
    put_u16(packet, &offset, 0); put_u16(packet, &offset, 0);
    if (!put_name(packet, &offset, capacity, hostname)) return 0;
    put_u16(packet, &offset, 1);       /* A */
    put_u16(packet, &offset, 0x8001);  /* cache flush + IN */
    put_u32(packet, &offset, 120);
    put_u16(packet, &offset, 4);
    memcpy(packet + offset, &address.s_addr, 4);
    offset += 4;
    return offset;
}

static size_t build_service_response(unsigned char *packet, size_t capacity,
                                     const char *hostname,
                                     struct in_addr address)
{
    const char *service = "LabTwin._http._tcp.local";
    size_t offset = 0;
    size_t length_position;
    size_t value_start;
    memset(packet, 0, capacity);
    put_u16(packet, &offset, 0); put_u16(packet, &offset, 0x8400);
    put_u16(packet, &offset, 0); put_u16(packet, &offset, 4);
    put_u16(packet, &offset, 0); put_u16(packet, &offset, 0);

    put_name(packet, &offset, capacity, "_http._tcp.local");
    put_u16(packet, &offset, 12); put_u16(packet, &offset, 1);
    put_u32(packet, &offset, 120); length_position = offset; put_u16(packet, &offset, 0);
    value_start = offset; put_name(packet, &offset, capacity, service);
    packet[length_position] = (unsigned char)((offset - value_start) >> 8);
    packet[length_position + 1] = (unsigned char)(offset - value_start);

    put_name(packet, &offset, capacity, service);
    put_u16(packet, &offset, 33); put_u16(packet, &offset, 0x8001);
    put_u32(packet, &offset, 120); length_position = offset; put_u16(packet, &offset, 0);
    value_start = offset; put_u16(packet, &offset, 0); put_u16(packet, &offset, 0);
    put_u16(packet, &offset, 80); put_name(packet, &offset, capacity, hostname);
    packet[length_position] = (unsigned char)((offset - value_start) >> 8);
    packet[length_position + 1] = (unsigned char)(offset - value_start);

    put_name(packet, &offset, capacity, service);
    put_u16(packet, &offset, 16); put_u16(packet, &offset, 0x8001);
    put_u32(packet, &offset, 120); put_u16(packet, &offset, 7);
    packet[offset++] = 6; memcpy(packet + offset, "path=/", 6); offset += 6;

    put_name(packet, &offset, capacity, hostname);
    put_u16(packet, &offset, 1); put_u16(packet, &offset, 0x8001);
    put_u32(packet, &offset, 120); put_u16(packet, &offset, 4);
    memcpy(packet + offset, &address.s_addr, 4); offset += 4;
    return offset;
}

static void *mdns_thread(void *arg)
{
    unsigned char query[MDNS_PACKET_MAX];
    unsigned char response[MDNS_PACKET_MAX];
    struct sockaddr_in peer;
    socklen_t peer_size;
    (void)arg;
    while (g_mdns_running) {
        ssize_t count;
        struct in_addr address;
        char hostname[64] = "labtwin.local";
        size_t response_size = 0;
        peer_size = sizeof(peer);
        count = recvfrom(g_mdns_fd, query, sizeof(query), 0,
                         (struct sockaddr *)&peer, &peer_size);
        if (count <= 0) {
            if (errno == EINTR) continue;
            if (!g_mdns_running) break;
            usleep(100000);
            continue;
        }
        /* Never treat an mDNS response as a query.  Without this guard the
         * socket can consume its own multicast response and create a packet
         * storm that eventually starves the whole board. */
        if (count < 12 || (query[2] & 0x80) != 0)
            continue;
        if (netlib_get_ipv4addr("wlan0", &address) != 0 ||
            address.s_addr == INADDR_ANY)
            continue;
        if (claw_config_get("device.name", hostname, sizeof(hostname)) == OK &&
            !strstr(hostname, ".local"))
            strncat(hostname, ".local", sizeof(hostname) - strlen(hostname) - 1);
        if (packet_has_name(query, (size_t)count, hostname))
            response_size = build_host_response(response, sizeof(response),
                                                hostname, address);
        else if (packet_has_name(query, (size_t)count, "_http._tcp.local"))
            response_size = build_service_response(response, sizeof(response),
                                                   hostname, address);
        if (response_size > 0) {
            struct sockaddr_in multicast = {
                .sin_family = AF_INET,
                .sin_port = htons(MDNS_PORT),
                .sin_addr.s_addr = inet_addr(MDNS_GROUP),
            };
            sendto(g_mdns_fd, response, response_size, 0,
                   (struct sockaddr *)&multicast, sizeof(multicast));
        }
    }
    return NULL;
}

int labtwin_mdns_start(void)
{
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    struct ip_mreq membership;
    struct in_addr interface_address = { 0 };
    unsigned char multicast_loop = 0;
    int attempt;
    int reuse = 1;
    pthread_t thread;
    pthread_attr_t attr;
    if (g_mdns_running)
        return 0;

    for (attempt = 0; attempt < 30; attempt++) {
        if (netlib_get_ipv4addr("wlan0", &interface_address) == 0 &&
            interface_address.s_addr != INADDR_ANY)
            break;
        usleep(100000);
    }
    if (interface_address.s_addr == INADDR_ANY)
        return -EAGAIN;

    g_mdns_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_mdns_fd < 0)
        return -1;
    setsockopt(g_mdns_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(g_mdns_fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(g_mdns_fd); g_mdns_fd = -1; return -1;
    }
    membership.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    membership.imr_interface = interface_address;
    if (setsockopt(g_mdns_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &membership, sizeof(membership)) != 0) {
        close(g_mdns_fd); g_mdns_fd = -1; return -1;
    }
    if (setsockopt(g_mdns_fd, IPPROTO_IP, IP_MULTICAST_IF,
                   &interface_address, sizeof(interface_address)) != 0) {
        close(g_mdns_fd); g_mdns_fd = -1; return -1;
    }
    setsockopt(g_mdns_fd, IPPROTO_IP, IP_MULTICAST_LOOP,
               &multicast_loop, sizeof(multicast_loop));
    g_mdns_running = true;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4096);
    if (pthread_create(&thread, &attr, mdns_thread, NULL) != 0) {
        pthread_attr_destroy(&attr); labtwin_mdns_stop(); return -1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

void labtwin_mdns_stop(void)
{
    g_mdns_running = false;
    if (g_mdns_fd >= 0) {
        close(g_mdns_fd);
        g_mdns_fd = -1;
    }
}
