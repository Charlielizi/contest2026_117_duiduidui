#include "infra/admin_auth.h"
#include "infra/config_store.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

/* KVDB stores the config key as persist.ai_agent.<key>.  R528 uses
 * NAME_MAX=32, so the previous labtwin.admin.auth key produced a 35-byte
 * filename and failed with ENAMETOOLONG. */
#define AUTH_CONFIG_KEY "admin.auth"
#define AUTH_LEGACY_CONFIG_KEY "labtwin.admin.auth"
/* Opt-in only for private bring-up images.  The compact key keeps the
 * resulting R528 KVDB filename below NAME_MAX.  Absent means normal password
 * authentication. */
#define AUTH_DEBUG_OPEN_CONFIG_KEY "auth.debug"
#define AUTH_COMPACT_RECORD_PREFIX "b1:"
#ifndef AUTH_LEGACY_FILE_PATH
#define AUTH_LEGACY_FILE_PATH "/data/labtwin/admin.auth"
#endif
#define AUTH_ITERATIONS 10000U
#define AUTH_SALT_SIZE 16
#define AUTH_HASH_SIZE 32
#define AUTH_RECORD_SIZE (AUTH_SALT_SIZE + AUTH_HASH_SIZE)
#define AUTH_RECORD_BASE64_SIZE (((AUTH_RECORD_SIZE + 2) / 3) * 4)
#define AUTH_COMPACT_VALUE_SIZE (sizeof(AUTH_COMPACT_RECORD_PREFIX) - 1 + \
                                 AUTH_RECORD_BASE64_SIZE + 1)
#define AUTH_MAX_SESSIONS 4
#define ADMIN_PASSWORD_MIN 6
#define AUTH_IDLE_SECONDS (30 * 60)
#define AUTH_ABSOLUTE_SECONDS (8 * 60 * 60)
#define LOGIN_WINDOW_SECONDS (10 * 60)
#define LOGIN_MAX_FAILURES 5
#define AUTH_KVDB_SETTLE_US 250000

typedef struct {
    bool used;
    char token[ADMIN_TOKEN_HEX + 1];
    char csrf[ADMIN_CSRF_HEX + 1];
    time_t created;
    time_t last_seen;
} auth_session_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static auth_session_t g_sessions[AUTH_MAX_SESSIONS];
static unsigned char g_salt[AUTH_SALT_SIZE];
static unsigned char g_verifier[AUTH_HASH_SIZE];
static bool g_paired;
static bool g_debug_open_admin;
static bool g_initialization_in_progress;
static unsigned int g_login_failures;
static time_t g_login_window;

static bool admin_password_valid(const char *password)
{
    size_t length;
    size_t index;

    if (!password)
        return false;

    length = strlen(password);
    if (length < ADMIN_PASSWORD_MIN || length > 128)
        return false;

    for (index = 0; index < length; index++) {
        if (password[index] < '0' || password[index] > '9')
            return false;
    }

    return true;
}

static time_t now_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int random_bytes(void *buffer, size_t size)
{
    size_t received = 0;
    while (received < size) {
        ssize_t count = getrandom((unsigned char *)buffer + received,
                                  size - received, GRND_NONBLOCK);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            memset(buffer, 0, size);
            return -EAGAIN;
        }
        received += (size_t)count;
    }
    return 0;
}

static void hex_encode(const unsigned char *input, size_t input_size,
                       char *output, size_t output_size)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (output_size < input_size * 2 + 1)
        return;
    for (i = 0; i < input_size; i++) {
        output[i * 2] = digits[input[i] >> 4];
        output[i * 2 + 1] = digits[input[i] & 15];
    }
    output[input_size * 2] = '\0';
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int hex_decode(const char *input, unsigned char *output, size_t size)
{
    size_t i;
    if (!input || strlen(input) != size * 2)
        return -1;
    for (i = 0; i < size; i++) {
        int high = hex_value(input[i * 2]);
        int low = hex_value(input[i * 2 + 1]);
        if (high < 0 || low < 0)
            return -1;
        output[i] = (unsigned char)((high << 4) | low);
    }
    return 0;
}

/* Keep the primary credential record compact across property backends.  The
 * R528 target accepts up to 255-byte property values; its observed setup
 * failure was the KVDB filename length, not this payload.  The 48-byte
 * salt+verifier payload below has an unpadded 64-character Base64 form. */
static int base64_value(char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

static int base64_encode_record(const unsigned char *input, size_t input_size,
                                char *output, size_t output_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t input_index;
    size_t output_index = 0;

    if (!input || !output || input_size % 3 != 0 ||
        output_size < input_size / 3 * 4 + 1)
        return -EINVAL;
    for (input_index = 0; input_index < input_size; input_index += 3) {
        output[output_index++] = alphabet[input[input_index] >> 2];
        output[output_index++] = alphabet[((input[input_index] & 0x03) << 4) |
                                         (input[input_index + 1] >> 4)];
        output[output_index++] = alphabet[((input[input_index + 1] & 0x0f) << 2) |
                                         (input[input_index + 2] >> 6)];
        output[output_index++] = alphabet[input[input_index + 2] & 0x3f];
    }
    output[output_index] = '\0';
    return 0;
}

static int base64_decode_record(const char *input, unsigned char *output,
                                size_t output_size)
{
    size_t input_size;
    size_t input_index;
    size_t output_index = 0;

    if (!input || !output || (input_size = strlen(input)) % 4 != 0 ||
        output_size != input_size / 4 * 3)
        return -EINVAL;
    for (input_index = 0; input_index < input_size; input_index += 4) {
        int first = base64_value(input[input_index]);
        int second = base64_value(input[input_index + 1]);
        int third = base64_value(input[input_index + 2]);
        int fourth = base64_value(input[input_index + 3]);

        if (first < 0 || second < 0 || third < 0 || fourth < 0)
            return -EINVAL;
        output[output_index++] = (unsigned char)((first << 2) | (second >> 4));
        output[output_index++] = (unsigned char)((second << 4) | (third >> 2));
        output[output_index++] = (unsigned char)((third << 6) | fourth);
    }
    return 0;
}

static int credentials_to_compact_record(const unsigned char *salt,
                                         const unsigned char *verifier,
                                         char *record, size_t record_size)
{
    unsigned char bytes[AUTH_RECORD_SIZE];
    size_t prefix_size = sizeof(AUTH_COMPACT_RECORD_PREFIX) - 1;
    int ret;

    if (!salt || !verifier || !record || record_size < AUTH_COMPACT_VALUE_SIZE)
        return -EINVAL;
    memcpy(bytes, salt, AUTH_SALT_SIZE);
    memcpy(bytes + AUTH_SALT_SIZE, verifier, AUTH_HASH_SIZE);
    memcpy(record, AUTH_COMPACT_RECORD_PREFIX, prefix_size);
    ret = base64_encode_record(bytes, sizeof(bytes), record + prefix_size,
                               record_size - prefix_size);
    memset(bytes, 0, sizeof(bytes));
    return ret;
}

static int credentials_from_compact_record(const char *record,
                                           unsigned char *salt_out,
                                           unsigned char *verifier_out)
{
    unsigned char bytes[AUTH_RECORD_SIZE];
    size_t prefix_size = sizeof(AUTH_COMPACT_RECORD_PREFIX) - 1;
    int ret;

    if (!record || !salt_out || !verifier_out ||
        strncmp(record, AUTH_COMPACT_RECORD_PREFIX, prefix_size) != 0 ||
        strlen(record) != AUTH_COMPACT_VALUE_SIZE - 1)
        return -EINVAL;
    ret = base64_decode_record(record + prefix_size, bytes, sizeof(bytes));
    if (ret == 0) {
        memcpy(salt_out, bytes, AUTH_SALT_SIZE);
        memcpy(verifier_out, bytes + AUTH_SALT_SIZE, AUTH_HASH_SIZE);
    }
    memset(bytes, 0, sizeof(bytes));
    return ret;
}

static int derive_password(const char *password, const unsigned char *salt,
                           unsigned char *output)
{
    size_t length = password ? strlen(password) : 0;
    /* Keep verification compatible with credentials created by older builds;
     * admin_password_valid() enforces the stronger rule for first setup. */
    if (length < 4 || length > 128)
        return -1;
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        (const unsigned char *)password, length, salt, AUTH_SALT_SIZE,
        AUTH_ITERATIONS, AUTH_HASH_SIZE, output);
}

static bool constant_equal(const unsigned char *left,
                           const unsigned char *right, size_t size)
{
    unsigned char difference = 0;
    size_t i;
    for (i = 0; i < size; i++)
        difference |= left[i] ^ right[i];
    return difference == 0;
}

static int credentials_from_json(const char *json, unsigned char *salt_out,
                                 unsigned char *verifier_out)
{
    cJSON *root;
    const char *salt;
    const char *verifier;

    if (!json || !salt_out || !verifier_out)
        return -EINVAL;
    root = cJSON_Parse(json);
    salt = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "salt")) : NULL;
    verifier = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "verifier")) : NULL;
    if (!salt || !verifier ||
        hex_decode(salt, salt_out, AUTH_SALT_SIZE) != 0 ||
        hex_decode(verifier, verifier_out, AUTH_HASH_SIZE) != 0) {
        cJSON_Delete(root);
        return -EINVAL;
    }
    cJSON_Delete(root);
    return 0;
}

static int persist_credentials(const unsigned char *salt,
                               const unsigned char *verifier)
{
    char record[AUTH_COMPACT_VALUE_SIZE];
    int ret = -1;
    if (!salt || !verifier)
        return -EINVAL;
    if (credentials_to_compact_record(salt, verifier, record,
                                      sizeof(record)) != 0)
        return -EINVAL;
    if (claw_config_set(AUTH_CONFIG_KEY, record) == OK)
        ret = 0;
    memset(record, 0, sizeof(record));
    return ret;
}

static int load_legacy_file_credentials(unsigned char *salt_out,
                                        unsigned char *verifier_out)
{
    char buffer[512];
    FILE *file = fopen(AUTH_LEGACY_FILE_PATH, "r");
    size_t count;

    if (!file)
        return -1;
    count = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[count] = '\0';
    return credentials_from_json(buffer, salt_out, verifier_out);
}

static int load_credentials(bool *migrate_credentials)
{
    char buffer[512];

    if (migrate_credentials)
        *migrate_credentials = false;
    if (claw_config_get(AUTH_CONFIG_KEY, buffer, sizeof(buffer)) == OK) {
        if (credentials_from_compact_record(buffer, g_salt, g_verifier) == 0)
            return 0;
        if (credentials_from_json(buffer, g_salt, g_verifier) == 0) {
            if (migrate_credentials)
                *migrate_credentials = true;
            return 0;
        }
    }
    if (claw_config_get(AUTH_LEGACY_CONFIG_KEY, buffer, sizeof(buffer)) == OK) {
        if (credentials_from_compact_record(buffer, g_salt, g_verifier) == 0 ||
            credentials_from_json(buffer, g_salt, g_verifier) == 0) {
            if (migrate_credentials)
                *migrate_credentials = true;
            return 0;
        }
    }
    if (load_legacy_file_credentials(g_salt, g_verifier) != 0)
        return -1;
    if (migrate_credentials)
        *migrate_credentials = true;
    return 0;
}

static void session_expire_locked(time_t now)
{
    int i;
    for (i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (g_sessions[i].used &&
            (now - g_sessions[i].last_seen > AUTH_IDLE_SECONDS ||
             now - g_sessions[i].created > AUTH_ABSOLUTE_SECONDS))
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
    }
}

static int create_session_locked(admin_session_view_t *view)
{
    unsigned char token[ADMIN_TOKEN_HEX / 2];
    unsigned char csrf[ADMIN_CSRF_HEX / 2];
    time_t now = now_monotonic();
    int slot = -1;
    int i;
    session_expire_locked(now);
    for (i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (!g_sessions[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;
        for (i = 1; i < AUTH_MAX_SESSIONS; i++)
            if (g_sessions[i].last_seen < g_sessions[slot].last_seen)
                slot = i;
    }
    if (random_bytes(token, sizeof(token)) || random_bytes(csrf, sizeof(csrf))) {
        memset(token, 0, sizeof(token));
        memset(csrf, 0, sizeof(csrf));
        return -EAGAIN;
    }
    memset(&g_sessions[slot], 0, sizeof(g_sessions[slot]));
    g_sessions[slot].used = true;
    g_sessions[slot].created = now;
    g_sessions[slot].last_seen = now;
    hex_encode(token, sizeof(token), g_sessions[slot].token,
               sizeof(g_sessions[slot].token));
    hex_encode(csrf, sizeof(csrf), g_sessions[slot].csrf,
               sizeof(g_sessions[slot].csrf));
    view->authenticated = true;
    snprintf(view->token, sizeof(view->token), "%s", g_sessions[slot].token);
    snprintf(view->csrf, sizeof(view->csrf), "%s", g_sessions[slot].csrf);
    return 0;
}

static const char *header_value(const char *request, const char *name,
                                char *output, size_t size)
{
    char pattern[64];
    const char *start;
    const char *end;
    size_t length;
    snprintf(pattern, sizeof(pattern), "\r\n%s: ", name);
    start = strcasestr(request, pattern);
    if (!start)
        return NULL;
    start += strlen(pattern);
    end = strstr(start, "\r\n");
    if (!end)
        return NULL;
    length = (size_t)(end - start);
    if (length >= size)
        return NULL;
    memcpy(output, start, length);
    output[length] = '\0';
    return output;
}

int admin_auth_init(void)
{
    char debug_open[8] = { 0 };
    bool migrate_credentials = false;
    int ret;

    config_store_init();
    if (claw_config_get(AUTH_DEBUG_OPEN_CONFIG_KEY, debug_open,
                        sizeof(debug_open)) != OK) {
#ifdef CONFIG_AI_AGENT_PRIVATE_OPEN_ADMIN
        /* A full PhoenixSuit erase recreates /data. Private laboratory
         * images bootstrap only this non-secret marker on the first service
         * start, rather than relying on an existing KVDB file or embedding
         * a credential in the image. */
        if (claw_config_set(AUTH_DEBUG_OPEN_CONFIG_KEY, "1") == OK) {
            usleep(AUTH_KVDB_SETTLE_US);
            snprintf(debug_open, sizeof(debug_open), "1");
            syslog(LOG_INFO,
                   "[admin-auth] private open-admin marker initialized\n");
        } else {
            syslog(LOG_ERR,
                   "[admin-auth] private open-admin marker persistence failed\n");
        }
#endif
    }
    g_debug_open_admin = debug_open[0] == '1' &&
        (debug_open[1] == '\0' ||
         isspace((unsigned char)debug_open[1]));
    ret = load_credentials(&migrate_credentials);
    pthread_mutex_lock(&g_lock);
    g_paired = ret == 0;
    g_initialization_in_progress = false;
    pthread_mutex_unlock(&g_lock);
    if (ret == 0 && migrate_credentials &&
        persist_credentials(g_salt, g_verifier) != 0) {
        syslog(LOG_WARNING,
               "[admin-auth] credential remains on a compatibility backend; KVDB migration failed\n");
    }
    if (g_debug_open_admin) {
        syslog(LOG_WARNING,
               "[admin-auth] private debug open-admin mode enabled\n");
    }
    return 0;
}

bool admin_auth_is_initialized(void)
{
    bool paired;
    pthread_mutex_lock(&g_lock);
    paired = g_paired || g_debug_open_admin;
    pthread_mutex_unlock(&g_lock);
    return paired;
}

/* Device Settings in existing board UI still invokes this symbol.  First
 * administrator setup is now available directly from the web portal, so the
 * retained compatibility hook deliberately has no effect. */
int admin_auth_open_pairing_window(void)
{
    return 0;
}

int admin_auth_initialize(const char *password, admin_session_view_t *session)
{
    unsigned char salt[AUTH_SALT_SIZE];
    unsigned char verifier[AUTH_HASH_SIZE];
    int ret = -1;

    if (!session || !admin_password_valid(password))
        return -EINVAL;
    syslog(LOG_INFO, "[admin-auth] administrator initialization started\n");
    if (random_bytes(salt, sizeof(salt)) != 0 ||
        derive_password(password, salt, verifier) != 0) {
        ret = -EIO;
        goto done;
    }
    syslog(LOG_INFO, "[admin-auth] password verifier derived\n");

    pthread_mutex_lock(&g_lock);
    if (g_paired || g_initialization_in_progress) {
        pthread_mutex_unlock(&g_lock);
        ret = -EACCES;
        goto done;
    }
    g_initialization_in_progress = true;
    pthread_mutex_unlock(&g_lock);

    /* KVDB/property writes may block.  Do not hold the session mutex while
     * committing the verifier, otherwise all authenticated requests stall. */
    if (persist_credentials(salt, verifier) != 0) {
        ret = -ENOSPC;
        goto finish;
    }
    syslog(LOG_INFO, "[admin-auth] password verifier queued for persistence\n");

    /* property_set() hands the file-backed KVDB update to its worker.  Do not
     * publish the authenticated session until the final NAND write has had a
     * chance to settle: the Portal immediately starts several protected
     * reads as soon as setup returns. */
    usleep(AUTH_KVDB_SETTLE_US);

    pthread_mutex_lock(&g_lock);
    memcpy(g_salt, salt, sizeof(g_salt));
    memcpy(g_verifier, verifier, sizeof(g_verifier));
    g_paired = true;
    ret = create_session_locked(session);
    pthread_mutex_unlock(&g_lock);
    if (ret == 0)
        syslog(LOG_INFO, "[admin-auth] administrator initialization complete\n");

finish:
    pthread_mutex_lock(&g_lock);
    g_initialization_in_progress = false;
    pthread_mutex_unlock(&g_lock);
done:
    memset(salt, 0, sizeof(salt));
    memset(verifier, 0, sizeof(verifier));
    return ret;
}

bool admin_auth_verify_password(const char *password)
{
    unsigned char candidate[AUTH_HASH_SIZE];
    bool valid = false;
    pthread_mutex_lock(&g_lock);
    if (g_paired && derive_password(password, g_salt, candidate) == 0)
        valid = constant_equal(candidate, g_verifier, sizeof(candidate));
    memset(candidate, 0, sizeof(candidate));
    pthread_mutex_unlock(&g_lock);
    return valid;
}

bool admin_auth_private_open(void)
{
    bool enabled;
    pthread_mutex_lock(&g_lock);
    enabled = g_debug_open_admin;
    pthread_mutex_unlock(&g_lock);
    return enabled;
}

bool admin_auth_token_valid(const char *token)
{
    bool valid = false;
    if (!token) return false;
    pthread_mutex_lock(&g_lock);
    session_expire_locked(now_monotonic());
    if (g_debug_open_admin && !strcmp(token, "debug-open-admin")) valid = true;
    else for (int i = 0; i < AUTH_MAX_SESSIONS; i++)
        if (g_sessions[i].used && !strcmp(g_sessions[i].token, token)) valid = true;
    pthread_mutex_unlock(&g_lock);
    return valid;
}

int admin_auth_login(const char *password, admin_session_view_t *session)
{
    unsigned char candidate[AUTH_HASH_SIZE];
    time_t now = now_monotonic();
    int ret = -1;
    pthread_mutex_lock(&g_lock);
    if (now - g_login_window > LOGIN_WINDOW_SECONDS) {
        g_login_window = now;
        g_login_failures = 0;
    }
    if (!g_paired || g_login_failures >= LOGIN_MAX_FAILURES)
        goto done;
    if (derive_password(password, g_salt, candidate) == 0 &&
        constant_equal(candidate, g_verifier, sizeof(candidate))) {
        g_login_failures = 0;
        ret = create_session_locked(session);
    } else {
        g_login_failures++;
    }
    memset(candidate, 0, sizeof(candidate));
done:
    pthread_mutex_unlock(&g_lock);
    return ret;
}

bool admin_auth_session(const char *request, admin_session_view_t *view)
{
    char cookie[512];
    const char *token;
    const char *end;
    char token_value[ADMIN_TOKEN_HEX + 1];
    time_t now = now_monotonic();
    int i;
    memset(view, 0, sizeof(*view));
    /* A private bring-up image may explicitly opt out of browser login.  It
     * still exposes a non-empty CSRF value so write routes retain their
     * same-origin/CSRF contract.  Production images have no such KVDB flag. */
    pthread_mutex_lock(&g_lock);
    if (g_debug_open_admin) {
        view->authenticated = true;
        snprintf(view->token, sizeof(view->token), "debug-open-admin");
        snprintf(view->csrf, sizeof(view->csrf), "debug-open-admin");
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    pthread_mutex_unlock(&g_lock);
    if (!header_value(request, "Cookie", cookie, sizeof(cookie)))
        return false;
    token = strstr(cookie, "labtwin_admin=");
    if (!token)
        return false;
    token += strlen("labtwin_admin=");
    end = strchr(token, ';');
    if (!end) end = token + strlen(token);
    if ((size_t)(end - token) != ADMIN_TOKEN_HEX)
        return false;
    memcpy(token_value, token, ADMIN_TOKEN_HEX);
    token_value[ADMIN_TOKEN_HEX] = '\0';
    pthread_mutex_lock(&g_lock);
    session_expire_locked(now);
    for (i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (g_sessions[i].used &&
            strcmp(g_sessions[i].token, token_value) == 0) {
            g_sessions[i].last_seen = now;
            view->authenticated = true;
            snprintf(view->token, sizeof(view->token), "%s", token_value);
            snprintf(view->csrf, sizeof(view->csrf), "%s", g_sessions[i].csrf);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return view->authenticated;
}

bool admin_auth_csrf_valid(const char *request,
                           const admin_session_view_t *session)
{
    char csrf[ADMIN_CSRF_HEX + 1];
    return session && session->authenticated &&
        header_value(request, "X-CSRF-Token", csrf, sizeof(csrf)) &&
        strcmp(csrf, session->csrf) == 0;
}

void admin_auth_logout(const char *request)
{
    admin_session_view_t view;
    int i;
    if (!admin_auth_session(request, &view))
        return;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < AUTH_MAX_SESSIONS; i++)
        if (g_sessions[i].used &&
            strcmp(g_sessions[i].token, view.token) == 0)
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
    pthread_mutex_unlock(&g_lock);
}
