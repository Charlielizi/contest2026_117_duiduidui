#include "infra/admin_auth.h"
#include "infra/config_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

#ifndef AUTH_LEGACY_FILE_PATH
#define AUTH_LEGACY_FILE_PATH "/tmp/labtwin-admin-auth-legacy-test.json"
#endif

#define TEST_SALT_SIZE 16
#define TEST_HASH_SIZE 32
#define TEST_ITERATIONS 10000U
#define TEST_PROPERTY_VALUE_MAX 96
#define TEST_KVDB_PATH_PREFIX "persist.ai_agent."
#define TEST_NAME_MAX 32

static char g_config_key[64];
static char g_config_value[512];
static int g_config_writes;

/* The Ubuntu host runner does not provide the firmware's mbedTLS libraries.
 * This deterministic test double exercises the credential and session state
 * machine; the target build still compiles against the real PBKDF2 code. */
int mbedtls_pkcs5_pbkdf2_hmac_ext(mbedtls_md_type_t md_type,
                                  const unsigned char *password,
                                  size_t password_length,
                                  const unsigned char *salt,
                                  size_t salt_length,
                                  unsigned int iterations,
                                  uint32_t output_length,
                                  unsigned char *output)
{
    uint32_t state = 2166136261U;
    size_t i;

    if (md_type != MBEDTLS_MD_SHA256 || !password || !salt || !output ||
        password_length == 0 || salt_length == 0 || iterations == 0)
        return -1;
    for (i = 0; i < password_length; i++)
        state = (state ^ password[i]) * 16777619U;
    for (i = 0; i < salt_length; i++)
        state = (state ^ salt[i]) * 16777619U;
    for (i = 0; i < iterations; i++)
        state = state * 1664525U + 1013904223U + (uint32_t)i;
    for (i = 0; i < output_length; i++) {
        state = state * 1664525U + 1013904223U;
        output[i] = (unsigned char)(state >> ((i % 4) * 8));
    }
    return 0;
}

void test_arc4random_buf(void *buffer, size_t size)
{
    unsigned char *bytes = buffer;
    size_t i;

    for (i = 0; i < size; i++)
        bytes[i] = (unsigned char)(i * 29U + 17U);
}

int config_store_init(void)
{
    return 0;
}

int claw_config_get(const char *key, char *buffer, size_t buffer_size)
{
    if (strcmp(key, g_config_key) != 0 || g_config_value[0] == '\0' ||
        strlen(g_config_value) >= buffer_size)
        return -1;
    snprintf(buffer, buffer_size, "%s", g_config_value);
    return 0;
}

int claw_config_set(const char *key, const char *value)
{
    assert(value != NULL);
    assert(strlen(value) < sizeof(g_config_value));
    if (strlen(TEST_KVDB_PATH_PREFIX) + strlen(key) > TEST_NAME_MAX)
        return -1;
    /* config_store.c adds the "v1:" prefix before calling property_set(). */
    if (strlen(value) + strlen("v1:") >= TEST_PROPERTY_VALUE_MAX)
        return -1;
    snprintf(g_config_key, sizeof(g_config_key), "%s", key);
    snprintf(g_config_value, sizeof(g_config_value), "%s", value);
    g_config_writes++;
    return 0;
}

static void format_test_pin(char output[7])
{
    snprintf(output, 7, "%06u", 246810U);
}

static void hex_encode(const unsigned char *input, size_t input_size,
                       char *output)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < input_size; i++) {
        output[i * 2] = digits[input[i] >> 4];
        output[i * 2 + 1] = digits[input[i] & 15];
    }
    output[input_size * 2] = '\0';
}

static void assert_session_round_trip(const char *pin,
                                      const admin_session_view_t *session)
{
    admin_session_view_t current;
    char request[256];

    assert(admin_auth_verify_password(pin));
    snprintf(request, sizeof(request),
             "GET /labtwin HTTP/1.1\r\nCookie: labtwin_admin=%s\r\n"
             "X-CSRF-Token: %s\r\n\r\n",
             session->token, session->csrf);
    assert(admin_auth_session(request, &current));
    assert(admin_auth_token_valid(session->token));
    assert(admin_auth_csrf_valid(request, &current));
    assert(!admin_auth_csrf_valid("POST /api/v2/agent/operations/id/confirm HTTP/1.1\r\n\r\n", &current));
    assert(!admin_auth_private_open());
    admin_auth_logout(request);
    assert(!admin_auth_token_valid(session->token));
    assert(!admin_auth_session(request, &current));
}

static void test_first_setup_and_login(void)
{
    admin_session_view_t session;
    char pin[7];

    format_test_pin(pin);
    assert(admin_auth_init() == 0);
    assert(!admin_auth_is_initialized());
    assert(admin_auth_open_pairing_window() == 0);
    assert(admin_auth_initialize("12345", &session) != 0);
    assert(admin_auth_initialize(pin, &session) == 0);
    assert(admin_auth_is_initialized());
    assert(admin_auth_initialize(pin, &session) != 0);
    assert(g_config_writes == 1);
    assert(strcmp(g_config_key, "admin.auth") == 0);
    assert(strncmp(g_config_value, "b1:", 3) == 0);
    assert(strlen(g_config_value) == 67);
    assert_session_round_trip(pin, &session);
    assert(admin_auth_login(pin, &session) == 0);
    assert_session_round_trip(pin, &session);
    assert(admin_auth_init() == 0);
    assert(admin_auth_is_initialized());
    assert(admin_auth_login(pin, &session) == 0);
    assert_session_round_trip(pin, &session);
}

static void write_legacy_credentials(const char *pin)
{
    unsigned char salt[TEST_SALT_SIZE];
    unsigned char verifier[TEST_HASH_SIZE];
    char salt_hex[TEST_SALT_SIZE * 2 + 1];
    char verifier_hex[TEST_HASH_SIZE * 2 + 1];
    FILE *file;
    size_t i;

    for (i = 0; i < sizeof(salt); i++)
        salt[i] = (unsigned char)(i + 1);
    assert(mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        (const unsigned char *)pin, strlen(pin), salt, sizeof(salt),
        TEST_ITERATIONS, sizeof(verifier), verifier) == 0);
    hex_encode(salt, sizeof(salt), salt_hex);
    hex_encode(verifier, sizeof(verifier), verifier_hex);
    file = fopen(AUTH_LEGACY_FILE_PATH, "w");
    assert(file != NULL);
    assert(fprintf(file, "{\"schema_version\":1,\"iterations\":%u,"
                   "\"salt\":\"%s\",\"verifier\":\"%s\"}",
                   TEST_ITERATIONS, salt_hex, verifier_hex) > 0);
    assert(fclose(file) == 0);
    memset(verifier, 0, sizeof(verifier));
}

static void test_legacy_migration(void)
{
    admin_session_view_t session;
    char pin[7];

    format_test_pin(pin);
    unlink(AUTH_LEGACY_FILE_PATH);
    write_legacy_credentials(pin);
    assert(admin_auth_init() == 0);
    assert(admin_auth_is_initialized());
    assert(g_config_writes == 1);
    assert(admin_auth_login(pin, &session) == 0);
    assert_session_round_trip(pin, &session);
    assert(unlink(AUTH_LEGACY_FILE_PATH) == 0);
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--legacy") == 0)
        test_legacy_migration();
    else {
        assert(argc == 1);
        test_first_setup_and_login();
    }
    return 0;
}
