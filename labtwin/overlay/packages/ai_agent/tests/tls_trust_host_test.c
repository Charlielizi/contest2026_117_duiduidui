#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
static const char *test_ca;
static bool unsynced;
static time_t test_time(time_t *out)
{
    time_t value = unsynced ? 0 : time(NULL);
    if (out) *out = value;
    return value;
}
#define TLS_CA_PATH test_ca
#define time test_time
#include "../src/infra/tls_trust.c"
#undef time
int main(int argc, char **argv)
{
    assert(argc == 5);
    test_ca = argv[1];
    int expected = atoi(argv[4]);
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_context entropy;
    mbedtls_ssl_config_init(&config); mbedtls_ssl_init(&ssl);
    mbedtls_net_init(&net); mbedtls_ctr_drbg_init(&rng); mbedtls_entropy_init(&entropy);
    assert(mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0);
    unsynced = true; assert(tls_trust_configure(&config) == TLS_TIME_UNSYNCED); unsynced = false;
    int ret = tls_trust_configure(&config);
    if (expected == 2) { assert(ret == TLS_TRUST_UNAVAILABLE); puts("tls_trust: missing/malformed roots rejected"); return 0; }
    assert(ret == 0);
    assert(mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy, NULL, 0) == 0);
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &rng);
    assert(mbedtls_ssl_setup(&ssl, &config) == 0);
    assert(mbedtls_ssl_set_hostname(&ssl, argv[2]) == 0);
    assert(mbedtls_net_connect(&net, "127.0.0.1", argv[3], MBEDTLS_NET_PROTO_TCP) == 0);
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
    do { ret = mbedtls_ssl_handshake(&ssl); } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    unsigned int flags = mbedtls_ssl_get_verify_result(&ssl);
    if (expected == 1) assert(ret == 0 && flags == 0);
    else assert(ret != 0 && flags != 0);
    printf("tls_trust_host_test: expected=%d ret=%d flags=%u PASS\n", expected, ret, flags);
    mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&config); mbedtls_net_free(&net);
    mbedtls_ctr_drbg_free(&rng); mbedtls_entropy_free(&entropy);
    mbedtls_x509_crt_free(&g_roots);
    return 0;
}
