#pragma once
#include <stdbool.h>

// ============================================================================
// Internet access control
//
// Two independent switches, because they stop different things:
//
//   WAN gate       NAT routing for hotspot clients. Off means the
//                  controller cannot reach anything past the bridge.
//   Cloud forward  Whether the proxy mirrors the controller's session to
//                  the real Spider Farmer cloud.
//
// Turning NAT off alone does NOT stop controller data reaching the
// vendor: the proxy opens its cloud connection over the bridge's own
// uplink, which bypasses NAT entirely. Both switches have to be off for
// a genuinely local-only setup.
//
// With cloud forwarding off the bridge answers the controller itself
// (CONNACK, SUBACK, PUBACK, PINGRESP). Without those replies the
// controller treats the session as dead and reconnects every few
// seconds, never settling long enough to publish anything.
//
// Home Assistant control keeps working either way — it never used the
// cloud path.
// ============================================================================

// Loads the stored state and applies it. Call once after Wi-Fi is up.
void wan_gate_init(void);

// NAT routing for hotspot clients.
bool wan_gate_is_open(void);
void wan_gate_set(bool open);

// Whether the proxy mirrors traffic to the vendor cloud. Takes effect on
// the next controller session; an active one is left alone.
bool wan_gate_cloud_forward(void);
void wan_gate_set_cloud_forward(bool enabled);
