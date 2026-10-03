#pragma once
#include <stdbool.h>
#include "mbedtls/ssl.h"
#define TLS_TRUST_UNAVAILABLE (-30001)
#define TLS_TIME_UNSYNCED (-30002)
bool tls_trust_clock_ready(void);
/* Configure REQUIRED verification against the immutable shared CA cache. */
int tls_trust_configure(mbedtls_ssl_config *config);
const char *tls_trust_status(void);
