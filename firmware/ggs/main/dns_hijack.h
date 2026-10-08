#pragma once
#include <stdbool.h>
#include <stddef.h>

// Enables or disables NAT routing on the AP interface.
//
// This is only half of the local-only story: the proxy's own connection
// to the Spider Farmer cloud runs over the bridge's uplink and bypasses
// NAT entirely. See wan_gate.h for the pair of switches.
void napt_set_enabled(bool enabled);

// Starts the DNS proxy on the AP interface, port 53.
//
// Always redirects sf.mqtt.spider-farmer.com to the bridge's own address,
// which is what makes the whole proxy work. Optionally redirects time
// server lookups, and enforces the offline exceptions.
void dns_hijack_start(void);

// Sets the upstream resolver for client lookups. Empty target means "use
// the resolver from the uplink's DHCP lease".
//
// Lookups are always FORWARDED and the resolver's own answer relayed
// back; the bridge never fabricates a reply. force_all simply makes
// every client use this resolver regardless of what DHCP handed out.
//
// The sole exception is the Spider Farmer cloud hostname, which is
// always answered with the bridge's own address — that redirect is what
// makes the MQTT proxy work.
void dns_set_target(const char *target, bool force_all);

// Sets where client time server lookups are pointed. Takes effect on the
// next query, no restart needed.
void dns_set_ntp_target(const char *target, bool enabled);

// Whether name resolution and time sync stay available while internet
// access is switched off.
//
// Read once and cached here on purpose: sb_prov_cfg_t is around 600
// bytes, and loading it inside the DNS task on every query overflowed
// that task's stack in an earlier version and sent the device into a
// boot loop.
void dns_set_offline_exceptions(bool allow_dns, bool allow_ntp);

// The resolver actually in use, as text, so the web interface can show
// what "from DHCP" resolved to. out needs 32 bytes.
void dns_effective_resolver(char *out, size_t out_sz);
