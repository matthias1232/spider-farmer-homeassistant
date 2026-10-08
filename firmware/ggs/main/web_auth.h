#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

// ============================================================================
// Web interface protection
//
// HTTP basic auth with a fixed user name ("admin") and a password stored
// in NVS. An empty password disables the login entirely, which is the
// default — the hotspot is already WPA2 protected, and locking a user out
// of their own bridge is worse than the exposure.
//
// Basic auth sends credentials base64-encoded, not encrypted. Over plain
// HTTP on a local hotspot that is acceptable; it keeps casual access out,
// not a determined attacker on the same network.
// ============================================================================

// Reloads the password from the stored configuration.
void web_auth_reload(void);

// true when a password is set and the web interface is protected.
bool web_auth_enabled(void);

// Checks the request. Returns true when it may proceed. On failure it has
// already sent a 401 response, so the handler must return immediately.
bool web_auth_check(httpd_req_t *req);

// Heavy JSON handlers take this: false (503 already sent) while another
// heavy request runs. Pair every true with web_heavy_end().
bool web_heavy_begin(httpd_req_t *req);
void web_heavy_end(void);

// Requests served, shed for low memory, refused as busy (since boot).
void web_stats(uint32_t *served, uint32_t *shed, uint32_t *busy);
