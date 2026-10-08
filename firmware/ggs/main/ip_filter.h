#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

// ============================================================================
// Web interface access by address
//
// Restricts which clients may reach the settings pages, alongside the
// password. Three modes, mirroring the hotspot MAC filter:
//
//   IPF_OFF        any address may connect
//   IPF_BLOCKLIST  any address except the listed ones
//   IPF_ALLOWLIST  only the listed ones
//
// Rules are CIDR, so "192.168.10.0/24" covers a whole subnet and
// "192.168.10.5/32" a single machine.
//
// One rule overrides everything: clients on the bridge's own hotspot are
// always allowed, whatever the list says. Without that, a typo in an
// allow-list would lock the interface away permanently and the only way
// back would be a serial flash. Joining the hotspot is always possible,
// so there is always a way in.
// ============================================================================

#define IPF_MAX_RULES 8
#define IPF_NOTE_LEN  24

typedef enum {
    IPF_OFF = 0,
    IPF_BLOCKLIST,
    IPF_ALLOWLIST,
} ip_filter_mode_t;

typedef struct {
    bool     in_use;
    uint32_t network;   // host byte order, already masked
    uint8_t  prefix;    // 0-32
    char     note[IPF_NOTE_LEN];
} ip_filter_rule_t;

void ip_filter_init(void);

ip_filter_mode_t ip_filter_get_mode(void);

// Refuses allow-list mode with no rules, which would block everyone.
bool ip_filter_set_mode(ip_filter_mode_t mode);

// Adds a rule from "192.168.1.0/24" or a bare address (treated as /32).
bool ip_filter_add(const char *cidr, const char *note);
bool ip_filter_remove(int index);

int ip_filter_count(void);
const ip_filter_rule_t *ip_filter_at(int index);

// Decides whether a request may proceed. Hotspot clients always may.
bool ip_filter_allows_request(httpd_req_t *req);

// Formats a rule as "192.168.1.0/24". out needs 20 bytes.
void ip_filter_format(const ip_filter_rule_t *r, char *out);
