/* Host regression compiles the actual NTP client; never sets the host clock. */
#include <pthread.h>
#include <stdio.h>
#include <time.h>
static int clock_sets;
static struct timespec applied;
static int capture_clock_settime(clockid_t clock, const struct timespec *ts)
{
  (void)clock;
  clock_sets++;
  applied = *ts;
  return 0;
}
#define clock_settime capture_clock_settime
#include "../../netutils/ntpclient/ntpclient.c"
#undef clock_settime

static int ping_calls;
int netlib_check_ipconnectivity(const char *ip, int timeout, int retry)
{
  (void)ip; (void)timeout; (void)retry;
  ping_calls++;
  return 0; /* DNS deliberately drops ICMP; UDP still works. */
}
int task_create(const char *name, int priority, int stack,
                int (*entry)(int, char **), char **argv)
{
  (void)name; (void)priority; (void)stack; (void)entry; (void)argv;
  errno = ENOSYS;
  return -1;
}

static int server;
static void *serve(void *arg)
{
  (void)arg;
  struct ntp_datagram_s request, reply = {0};
  struct sockaddr_in peer;
  socklen_t len = sizeof(peer);
  assert(recvfrom(server, &request, sizeof(request), 0,
                  (struct sockaddr *)&peer, &len) == sizeof(request));
  reply.lvm = MKLVM(0, 4, 4);
  reply.stratum = 2;
  memcpy(reply.origtimestamp, request.xmittimestamp, 8);
  uint64_t now = ntp_localtime();
  ntpc_setuint64(reply.recvtimestamp, now);
  ntpc_setuint64(reply.xmittimestamp, now);
  ntpc_setuint64(reply.reftimestamp, now - ((uint64_t)60 << 32));
  assert(sendto(server, &reply, sizeof(reply), 0,
                (struct sockaddr *)&peer, len) == sizeof(reply));
  return NULL;
}

int main(void)
{
  sem_init(&g_ntpc_daemon.lock, 0, 1);
  sem_init(&g_ntpc_daemon.sync, 0, 0);
  int fd = ntpc_create_dgram_socket(AF_INET);
  assert(fd >= 0);
  struct timeval timeout;
  socklen_t length = sizeof(timeout);
  assert(getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, &length) == 0);
  assert(timeout.tv_sec == 1 && timeout.tv_usec == 250000);
  close(fd);

  server = socket(AF_INET, SOCK_DGRAM, 0);
  assert(server >= 0);
  struct sockaddr_in addr = { .sin_family = AF_INET,
    .sin_port = htons(CONFIG_NETUTILS_NTPCLIENT_PORTNO),
    .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
  assert(bind(server, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, serve, NULL) == 0);
  char *argv[] = { "ntp-test", "127.0.0.1", NULL };
  assert(ntpc_daemon(2, argv) == EXIT_SUCCESS);
  pthread_join(thread, NULL);
  close(server);
  assert(ping_calls == 0 && clock_sets == 1 && g_last_nsamples == 1);
  assert(llabs((long long)applied.tv_sec - time(NULL)) < 2);

  struct ntp_datagram_s request = {0}, response = {0};
  union ntp_addr_u peer = {0};
  peer.in4 = addr;
  response.lvm = MKLVM(0, 4, 4); response.stratum = 2;
  uint64_t now = ntp_localtime();
  ntpc_setuint64(request.xmittimestamp, now);
  memcpy(response.origtimestamp, request.xmittimestamp, 8);
  ntpc_setuint64(response.reftimestamp, now);
  ntpc_setuint64(response.recvtimestamp, now);
  ntpc_setuint64(response.xmittimestamp, now);
  assert(ntpc_verify_recvd_ntp_datagram(&request, &response, sizeof(response), &peer, &peer, sizeof(addr)));
  response.origtimestamp[0] ^= 1;
  assert(!ntpc_verify_recvd_ntp_datagram(&request, &response, sizeof(response), &peer, &peer, sizeof(addr)));
  response.origtimestamp[0] ^= 1;
  response.lvm = MKLVM(3, 4, 4);
  assert(!ntpc_verify_recvd_ntp_datagram(&request, &response, sizeof(response), &peer, &peer, sizeof(addr)));
  response.lvm = MKLVM(0, 4, 3);
  assert(!ntpc_verify_recvd_ntp_datagram(&request, &response, sizeof(response), &peer, &peer, sizeof(addr)));
  response.lvm = MKLVM(0, 4, 4);
  assert(!ntpc_verify_recvd_ntp_datagram(&request, &response, 10, &peer, &peer, sizeof(addr)));
  union ntp_addr_u wrong = peer; wrong.in4.sin_port++;
  assert(!ntpc_verify_recvd_ntp_datagram(&request, &response, sizeof(response), &peer, &wrong, sizeof(addr)));
  printf("PASS actual NTP client: blocked ICMP, UDP sample, timeout units, wrong origin/peer/mode, unsynced server, short packet\n");
  return 0;
}
