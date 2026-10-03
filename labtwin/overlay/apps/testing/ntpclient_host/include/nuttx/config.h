#pragma once
#include <assert.h>
#include <semaphore.h>
#include <signal.h>
#define FAR
#define OK 0
#define ERROR -1
#define CONFIG_HAVE_LONG_LONG 1
#define CONFIG_LIBC_NETDB 1
#define CONFIG_NET_IPv4 1
#define CONFIG_NETUTILS_NTPCLIENT_SERVER "127.0.0.1"
#define CONFIG_NETUTILS_NTPCLIENT_PORTNO 30123
#define CONFIG_NETUTILS_NTPCLIENT_NUM_SAMPLES 1
#define CONFIG_NETUTILS_NTPCLIENT_TIMEOUT_MS 1250
#define CONFIG_NETUTILS_NTPCLIENT_RETRIES 0
#define SEM_INITIALIZER(value) {0}
#define DEBUGASSERT assert
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
typedef struct sq_entry_s { struct sq_entry_s *next; } sq_entry_t;
typedef struct { sq_entry_t *head, *tail; } sq_queue_t;
static inline sq_entry_t *sq_peek(sq_queue_t *q) { return q->head; }
static inline sq_entry_t *sq_next(sq_entry_t *e) { return e->next; }
static inline void sq_addlast(sq_entry_t *e, sq_queue_t *q) {
  e->next = 0;
  if (q->tail) q->tail->next = e; else q->head = e;
  q->tail = e;
}
