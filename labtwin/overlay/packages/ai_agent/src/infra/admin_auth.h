#pragma once

#include <stdbool.h>
#include <stddef.h>

#define ADMIN_TOKEN_HEX 64
#define ADMIN_CSRF_HEX 32

typedef struct {
    bool authenticated;
    char token[ADMIN_TOKEN_HEX + 1];
    char csrf[ADMIN_CSRF_HEX + 1];
} admin_session_view_t;

int admin_auth_init(void);
bool admin_auth_is_initialized(void);
int admin_auth_open_pairing_window(void);
int admin_auth_initialize(const char *password, admin_session_view_t *session);
int admin_auth_login(const char *password, admin_session_view_t *session);
bool admin_auth_session(const char *request, admin_session_view_t *session);
bool admin_auth_csrf_valid(const char *request,
                           const admin_session_view_t *session);
bool admin_auth_verify_password(const char *password);
bool admin_auth_private_open(void);
bool admin_auth_token_valid(const char *token);
void admin_auth_logout(const char *request);
