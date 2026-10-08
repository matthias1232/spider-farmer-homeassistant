#pragma once
#include <stdbool.h>
#include <stddef.h>

// Starts the TLS MITM proxy: listens on SB_LOCAL_TLS_PORT, terminates the
// controller's TLS connection (no client certificate required — the real
// controller does not present one), optionally mirrors traffic 1:1 to the
// real Spider Farmer cloud, and forwards controller data to Home
// Assistant.
void mitm_proxy_start(void);

// Injects a command into the active controller session. The payload is
// sent verbatim as a PUBLISH on the controller's DOWN topic.
void mitm_proxy_inject_command(const char *payload, size_t len);

// Same, addressed to one specific controller by MAC. With several
// connected this is the one to use; an empty MAC falls back to the first
// active session.
// Returns false when there is no session for it or it is too large.
bool mitm_proxy_inject_command_to(const char *mac, const char *payload, size_t len);

// Tells the vendor cloud what a controller's calibration offsets now are,
// as a synthetic getConfigField reply on the UP channel — the shape the
// cloud actually expects there, as opposed to a copy of the DOWN command
// (which was tried and made the session less stable, not more).
//
// Called once per calibration change, right after the command that sets
// it. cal_json is the {"temp":...,"humi":...,"co2":...,"ppfd":...} block.
void mitm_proxy_report_calibration(const char *mac, const char *cal_json);

// True when a calibration change for this device came too soon after the
// last one (see mitm_proxy_report_calibration() for why this exists).
// Checking this before building or sending a calibration command is how
// both the DOWN command and the UP mirror are skipped together for a
// rapid-fire burst, rather than each separately.
bool mitm_proxy_calibration_throttled(const char *mac);

// true while that particular controller has a session.
bool mitm_proxy_device_online(const char *mac);

// Ends every active controller session.
//
// Used when a setting changes that is only read when a session starts —
// cloud mirroring above all. The controller reconnects on its own within
// a few seconds, so this applies the new mode straight away instead of
// leaving it pending until the next reconnect or a reboot.
void mitm_proxy_drop_sessions(const char *why);

// uid of one specific controller, empty when it is not connected.
const char *mitm_proxy_uid_for(const char *mac);

// Keepalive health for one controller, for the status page.
//
// silence_s is how long since anything was heard from it, keepalive_s
// the interval it was observed to use (0 while still being learned).
// Together these show whether a quiet device is idle or actually gone.
void mitm_proxy_liveness(const char *mac, uint32_t *silence_s,
                         uint32_t *keepalive_s);

// Identity of the connected controller, learned from its UP topic.
// Returns an empty string when no session is active; command translation
// needs both to build a valid setConfigField.
const char *mitm_proxy_controller_mac(void);
const char *mitm_proxy_controller_uid(void);

// true while a controller session is up.
bool mitm_proxy_has_session(void);

// True while a cloud-leg reconnect (DNS + TLS handshake) is running.
bool mitm_proxy_cloud_reconnecting(void);

// Fills in the values that are not part of the module cache: air and
// soil sensor readings, and the outlet states. The control page uses
// this to show live data without re-parsing the raw stream itself.
//
// sensors receives name/value string pairs, outlets an array of
// {n, on} objects. Both must be existing cJSON containers.
//
// mac selects which controller's readings are returned, since each one
// has its own.
void mitm_proxy_fill_live_state(const char *mac, void *sensors_obj,
                                void *outlets_arr);
