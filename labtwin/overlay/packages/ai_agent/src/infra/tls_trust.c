#include "infra/tls_trust.h"
#include "mbedtls/x509_crt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#ifdef CONFIG_NETUTILS_NTPCLIENT
#include <netutils/ntpclient.h>
#endif
#endif
#ifndef TLS_CA_PATH
#define TLS_CA_PATH "/resource/etc/ssl/curl/ca-certificates.crt"
#endif
static pthread_mutex_t g_trust_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_x509_crt g_roots;
static bool g_loaded;
static int g_error;

bool tls_trust_clock_ready(void)
{
    if (time(NULL) < 1735689600) return false;
#if defined(__NuttX__) && defined(CONFIG_NETUTILS_NTPCLIENT)
    struct ntpc_status_s status;
    if (ntpc_status(&status) != 0 || status.nsamples == 0) return false;
#endif
    return true;
}

int tls_trust_configure(mbedtls_ssl_config *config)
{
    int ret = 0;
    if (!tls_trust_clock_ready()) return TLS_TIME_UNSYNCED;
    pthread_mutex_lock(&g_trust_lock);
    if (!g_loaded) {
        FILE *file = fopen(TLS_CA_PATH, "rb");
        unsigned char *pem = NULL;
        long size = 0;
        mbedtls_x509_crt_init(&g_roots);
        if (file && fseek(file, 0, SEEK_END) == 0) {
            size = ftell(file);
            rewind(file);
            if (size > 0 && size <= 512 * 1024) pem = malloc((size_t)size + 1);
        }
        if (!pem || fread(pem, 1, (size_t)size, file) != (size_t)size) ret = TLS_TRUST_UNAVAILABLE;
        else {
            pem[size] = 0;
            if (mbedtls_x509_crt_parse(&g_roots, pem, (size_t)size + 1) != 0)
                ret = TLS_TRUST_UNAVAILABLE;
        }
        free(pem);
        if (file) fclose(file);
        if (ret) mbedtls_x509_crt_free(&g_roots);
        g_error = ret;
        g_loaded = true;
    }
    ret = g_error;
    if (!ret) {
        mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(config, &g_roots, NULL);
    }
    pthread_mutex_unlock(&g_trust_lock);
    return ret;
}

const char *tls_trust_status(void)
{
    const char *status;
    if (!tls_trust_clock_ready()) return "TLS_TIME_UNSYNCED";
    pthread_mutex_lock(&g_trust_lock);
    status = g_error ? "TLS_TRUST_UNAVAILABLE" : g_loaded ? "TLS_READY" : "TLS_NOT_LOADED";
    pthread_mutex_unlock(&g_trust_lock);
    return status;
}
